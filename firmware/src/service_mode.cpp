#include "service_mode.h"

#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <stdarg.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#define SKIMMERSENSE_HAS_WIFI_SECRETS 1
#else
#define SKIMMERSENSE_HAS_WIFI_SECRETS 0
#endif

namespace {

static constexpr uint32_t RTC_DIAG_MAGIC = 0x534B4431UL;   // "SKD1"
static constexpr uint32_t RESET_LOG_MAGIC = 0x534B4C31UL;  // "SKL1"
static constexpr uint8_t RESET_LOG_CAPACITY = 8;
static constexpr uint32_t HOME_WIFI_TIMEOUT_MS = 15000UL;
static constexpr char MDNS_HOSTNAME[] = "skimmersense";
static constexpr uint32_t CYCLE_LOG_MAGIC = 0x534B4331UL;  // "SKC1"
static constexpr size_t CYCLE_LOG_CAPACITY = 3072;
static constexpr size_t SERVICE_LOG_CAPACITY = 8192;
static constexpr uint8_t SCENARIO_MAX_CYCLES = 50;
static constexpr size_t SCENARIO_LOG_MAX_BYTES = 196608;
static constexpr char CAPTURE_REMAINING_KEY[] = "capremain";
static constexpr char CAPTURE_COUNT_KEY[] = "capcount";
static constexpr char SCENARIO_LOG_PATH[] = "/scenario.log";

struct RtcDiagnostics {
  uint32_t magic = 0;
  uint32_t cycleCounter = 0;
  uint8_t stage = static_cast<uint8_t>(SkmDiagStage::BOOT);
  uint8_t state = 0;
  uint8_t nextState = 0;
  uint8_t lowClosed = 0;
  uint8_t highClosed = 0;
  uint8_t zigbeeAttempted = 0;
  uint8_t zigbeeConnectedKnown = 0;
  uint8_t zigbeeConnected = 0;
  uint8_t reportsKnown = 0;
  uint8_t reportsOk = 0;
  uint32_t sleepSeconds = 0;
};

struct PersistedResetEvent {
  uint32_t sequence = 0;
  uint8_t resetReason = 0;
  uint8_t priorRtcValid = 0;
  uint8_t stage = 0;
  uint8_t state = 0;
  uint8_t nextState = 0;
  uint8_t lowClosed = 0;
  uint8_t highClosed = 0;
  uint8_t zigbeeAttempted = 0;
  uint8_t zigbeeConnectedKnown = 0;
  uint8_t zigbeeConnected = 0;
  uint8_t reportsKnown = 0;
  uint8_t reportsOk = 0;
  uint32_t sleepSeconds = 0;
};

struct PersistedResetLog {
  uint32_t magic = RESET_LOG_MAGIC;
  uint32_t nextSequence = 1;
  uint8_t count = 0;
  uint8_t head = 0;
  uint8_t reserved[2] = {0, 0};
  PersistedResetEvent events[RESET_LOG_CAPACITY];
};

struct RetainedCycleLog {
  uint32_t magic;
  uint16_t length;
  uint8_t complete;
  uint8_t truncated;
  char text[CYCLE_LOG_CAPACITY];
};

RTC_DATA_ATTR RtcDiagnostics rtcDiagnostics;
RTC_NOINIT_ATTR RetainedCycleLog retainedCycleLog;
String serviceSessionLog;

void appendServiceSessionLine(const String &line) {
  String entry = line;
  if (!entry.endsWith("\n")) entry += '\n';

  if (entry.length() >= SERVICE_LOG_CAPACITY) {
    entry = entry.substring(entry.length() - SERVICE_LOG_CAPACITY + 1);
  }
  const size_t required = serviceSessionLog.length() + entry.length();
  if (required >= SERVICE_LOG_CAPACITY) {
    serviceSessionLog.remove(0, required - SERVICE_LOG_CAPACITY + 1);
  }
  serviceSessionLog += entry;
}

String combinedLogText() {
  String text;
  const String persisted = skmPersistedCycleLogSnapshot();
  text.reserve(CYCLE_LOG_CAPACITY + persisted.length() +
               serviceSessionLog.length() + 384);
  text += F("=== LAST RETAINED PRODUCTION CYCLE ===\n");
  if (!skmCycleLogAvailable()) {
    text += F("No retained production-cycle log. A power loss, USB flash or first boot may have cleared it.\n");
  } else {
    text += skmCycleLogIsComplete()
              ? F("Status: complete (deep sleep reached)\n")
              : F("Status: INCOMPLETE (cycle interrupted before deep sleep)\n");
    text += skmCycleLogSnapshot();
  }

  text += F("\n=== LAST PERSISTED SCENARIO CAPTURE ===\n");
  text += persisted.length()
            ? persisted
            : String(F("No persistent production scenario has been saved yet.\n"));

  text += F("\nScenario capture: ");
  if (skmCycleCaptureRequested()) {
    text += F("ARMED / ");
    text += String(skmCycleCaptureRemaining());
    text += F(" cycle(s) remaining\n");
  } else {
    text += F("not armed\n");
  }

  text += F("\n=== CURRENT SERVICE SESSION ===\n");
  text += serviceSessionLog.length()
            ? serviceSessionLog
            : String(F("No SERVICE event recorded yet.\n"));
  return text;
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL_RESET";
    case ESP_RST_SW: return "SOFTWARE_RESET";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "OTHER";
  }
}

PersistedResetLog loadResetLog() {
  PersistedResetLog log;
  Preferences prefs;
  if (!prefs.begin("skm_diag", true)) return log;

  const size_t storedSize = prefs.getBytesLength("resetlog");
  if (storedSize == sizeof(PersistedResetLog)) {
    PersistedResetLog candidate;
    if (prefs.getBytes("resetlog", &candidate, sizeof(candidate)) == sizeof(candidate) &&
        candidate.magic == RESET_LOG_MAGIC &&
        candidate.count <= RESET_LOG_CAPACITY &&
        candidate.head < RESET_LOG_CAPACITY) {
      log = candidate;
    }
  }
  prefs.end();
  return log;
}

void saveResetLog(const PersistedResetLog &log) {
  Preferences prefs;
  if (!prefs.begin("skm_diag", false)) return;
  prefs.putBytes("resetlog", &log, sizeof(log));
  prefs.end();
}

void appendResetEvent(esp_reset_reason_t reason,
                      bool priorRtcValid,
                      const RtcDiagnostics &prior) {
  PersistedResetLog log = loadResetLog();
  PersistedResetEvent event;
  event.sequence = log.nextSequence++;
  event.resetReason = static_cast<uint8_t>(reason);
  event.priorRtcValid = priorRtcValid ? 1 : 0;

  if (priorRtcValid) {
    event.stage = prior.stage;
    event.state = prior.state;
    event.nextState = prior.nextState;
    event.lowClosed = prior.lowClosed;
    event.highClosed = prior.highClosed;
    event.zigbeeAttempted = prior.zigbeeAttempted;
    event.zigbeeConnectedKnown = prior.zigbeeConnectedKnown;
    event.zigbeeConnected = prior.zigbeeConnected;
    event.reportsKnown = prior.reportsKnown;
    event.reportsOk = prior.reportsOk;
    event.sleepSeconds = prior.sleepSeconds;
  }

  log.events[log.head] = event;
  log.head = static_cast<uint8_t>((log.head + 1) % RESET_LOG_CAPACITY);
  if (log.count < RESET_LOG_CAPACITY) ++log.count;
  saveResetLog(log);
}

String yesNoUnknown(uint8_t known, uint8_t value) {
  if (!known) return "unknown";
  return value ? "yes" : "no";
}

String resetLogHtml() {
  const PersistedResetLog log = loadResetLog();
  String html;
  html.reserve(2600);
  html += F("<h2>Persistent reset history</h2>");
  if (log.count == 0) {
    html += F("<p>No non-deep-sleep reset has been recorded yet.</p>");
    return html;
  }

  html += F("<table><tr><th>#</th><th>Reset</th><th>Last stage</th><th>State</th><th>Next</th><th>LOW/HIGH</th><th>ZB</th><th>Reports</th><th>Sleep</th></tr>");
  for (uint8_t i = 0; i < log.count; ++i) {
    const uint8_t index = static_cast<uint8_t>(
        (log.head + RESET_LOG_CAPACITY - 1 - i) % RESET_LOG_CAPACITY);
    const PersistedResetEvent &event = log.events[index];

    html += F("<tr><td>"); html += String(event.sequence);
    html += F("</td><td>"); html += resetReasonName(static_cast<esp_reset_reason_t>(event.resetReason));
    html += F("</td>");
    if (!event.priorRtcValid) {
      html += F("<td colspan='7'>No retained RTC snapshot (power loss/cold boot possible)</td></tr>");
      continue;
    }
    html += F("<td>"); html += skmDiagStageName(static_cast<SkmDiagStage>(event.stage));
    html += F("</td><td>"); html += String(event.state);
    html += F("</td><td>"); html += String(event.nextState);
    html += F("</td><td>"); html += event.lowClosed ? F("CLOSED/") : F("OPEN/");
    html += event.highClosed ? F("CLOSED") : F("OPEN");
    html += F("</td><td>"); html += yesNoUnknown(event.zigbeeConnectedKnown, event.zigbeeConnected);
    html += F("</td><td>"); html += yesNoUnknown(event.reportsKnown, event.reportsOk);
    html += F("</td><td>"); html += String(event.sleepSeconds);
    html += F(" s</td></tr>");
  }
  html += F("</table>");
  return html;
}

String diagnosticsHtml() {
  String html;
  html.reserve(1800);
  html += F("<h2>RTC diagnostics</h2><table>");
  html += F("<tr><th>Current reset reason</th><td>");
  html += resetReasonName(esp_reset_reason());
  html += F(" ("); html += String(static_cast<int>(esp_reset_reason())); html += F(")</td></tr>");
  html += F("<tr><th>Retained cycle counter</th><td>"); html += String(rtcDiagnostics.cycleCounter); html += F("</td></tr>");
  html += F("<tr><th>Last stage</th><td>"); html += skmDiagStageName(static_cast<SkmDiagStage>(rtcDiagnostics.stage)); html += F("</td></tr>");
  html += F("<tr><th>State / next</th><td>"); html += String(rtcDiagnostics.state); html += F(" / "); html += String(rtcDiagnostics.nextState); html += F("</td></tr>");
  html += F("<tr><th>LOW / HIGH</th><td>"); html += rtcDiagnostics.lowClosed ? F("CLOSED / ") : F("OPEN / "); html += rtcDiagnostics.highClosed ? F("CLOSED") : F("OPEN"); html += F("</td></tr>");
  html += F("<tr><th>Zigbee attempted</th><td>"); html += rtcDiagnostics.zigbeeAttempted ? F("yes") : F("no"); html += F("</td></tr>");
  html += F("<tr><th>Zigbee connected</th><td>"); html += yesNoUnknown(rtcDiagnostics.zigbeeConnectedKnown, rtcDiagnostics.zigbeeConnected); html += F("</td></tr>");
  html += F("<tr><th>Reports OK</th><td>"); html += yesNoUnknown(rtcDiagnostics.reportsKnown, rtcDiagnostics.reportsOk); html += F("</td></tr>");
  html += F("<tr><th>Planned sleep</th><td>"); html += String(rtcDiagnostics.sleepSeconds); html += F(" s</td></tr></table>");
  return html;
}

String pageHeader(const char *firmwareVersion,
                  const char *firmwareFlavor,
                  const String &apSsid,
                  const String &apPassword,
                  const IPAddress &apIp,
                  bool homeConfigured,
                  const String &homeSsid,
                  bool staConnected,
                  const IPAddress &staIp,
                  bool mdnsOk) {
  String html;
  html.reserve(3400);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>SkimmerSense Service</title><style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;max-width:900px;margin:auto;padding:18px;background:#f4f6f8;color:#17202a}h1{margin-bottom:4px}h2{margin-top:28px}table{border-collapse:collapse;width:100%;background:white}th,td{border:1px solid #d8dde3;padding:7px;text-align:left}th{background:#eef2f5}code{background:#e8edf1;padding:2px 5px;border-radius:4px}.card{background:white;padding:14px;border-radius:10px;margin:12px 0}.warn{background:#fff4d6;padding:12px;border-radius:8px}.ok{background:#e7f6e7;padding:12px;border-radius:8px}button,input[type=submit]{font-size:16px;padding:9px 14px}</style></head><body>");
  html += F("<h1>SkimmerSense Service</h1><p>Firmware <strong>");
  html += firmwareVersion; html += F("</strong> — "); html += firmwareFlavor; html += F("</p>");

  html += F("<div class='card'><strong>Fallback maintenance AP</strong><br>SSID: <code>");
  html += apSsid; html += F("</code><br>Password: <code>"); html += apPassword;
  html += F("</code><br>Address: <code>http://"); html += apIp.toString(); html += F("/</code></div>");

  html += F("<div class='card'><strong>Home Wi-Fi</strong><br>");
  if (!homeConfigured) {
    html += F("Not configured. Create <code>firmware/include/wifi_secrets.h</code> from the example file.");
  } else {
    html += F("SSID: <code>"); html += homeSsid; html += F("</code><br>Status: ");
    if (staConnected) {
      html += F("<strong>connected</strong><br>LAN address: <code>http://");
      html += staIp.toString(); html += F("/</code><br>mDNS: ");
      if (mdnsOk) html += F("<code>http://skimmersense.local/</code>");
      else html += F("unavailable; use the LAN IP above");
    } else {
      html += F("<strong>not connected</strong>. The fallback AP remains available.");
    }
  }
  html += F("</div>");
  return html;
}

String pageFooter() { return F("</body></html>"); }

}  // namespace

void skmCycleLogBegin() {
  retainedCycleLog.magic = CYCLE_LOG_MAGIC;
  retainedCycleLog.length = 0;
  retainedCycleLog.complete = 0;
  retainedCycleLog.truncated = 0;
  retainedCycleLog.text[0] = '\0';
}

void skmCycleLogAppend(const char *format, ...) {
  if (retainedCycleLog.magic != CYCLE_LOG_MAGIC) skmCycleLogBegin();

  char line[384];
  va_list args;
  va_start(args, format);
  const int formatted = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (formatted < 0) return;

  const size_t lineLength =
      static_cast<size_t>(formatted) < sizeof(line)
        ? static_cast<size_t>(formatted)
        : sizeof(line) - 1;
  const size_t available =
      CYCLE_LOG_CAPACITY - 1 - retainedCycleLog.length;
  if (available == 0) {
    retainedCycleLog.truncated = 1;
    return;
  }

  const size_t copyLength = lineLength < available ? lineLength : available;
  memcpy(retainedCycleLog.text + retainedCycleLog.length, line, copyLength);
  retainedCycleLog.length += static_cast<uint16_t>(copyLength);

  if (copyLength < lineLength) {
    retainedCycleLog.truncated = 1;
  } else if (retainedCycleLog.length < CYCLE_LOG_CAPACITY - 1) {
    retainedCycleLog.text[retainedCycleLog.length++] = '\n';
  }
  retainedCycleLog.text[retainedCycleLog.length] = '\0';
}

void skmCycleLogComplete() {
  if (retainedCycleLog.magic == CYCLE_LOG_MAGIC) retainedCycleLog.complete = 1;
}

bool skmCycleLogAvailable() {
  return retainedCycleLog.magic == CYCLE_LOG_MAGIC &&
         retainedCycleLog.length > 0 &&
         retainedCycleLog.length < CYCLE_LOG_CAPACITY;
}

bool skmCycleLogIsComplete() {
  return skmCycleLogAvailable() && retainedCycleLog.complete != 0;
}

String skmCycleLogSnapshot() {
  if (!skmCycleLogAvailable()) return String();
  String text(retainedCycleLog.text);
  if (retainedCycleLog.truncated) {
    text += F("\n[production-cycle log truncated]\n");
  }
  return text;
}

bool skmRequestNextCycleCapture() {
  if (!SPIFFS.begin(true)) return false;
  if (SPIFFS.exists(SCENARIO_LOG_PATH) &&
      !SPIFFS.remove(SCENARIO_LOG_PATH)) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin("skm_diag", false)) return false;
  prefs.remove("capnext");
  prefs.remove("cyclelog");
  prefs.remove("scenlog");
  const bool countOk = prefs.putUChar(CAPTURE_COUNT_KEY, 0) > 0;
  const bool remainingOk =
      prefs.putUChar(CAPTURE_REMAINING_KEY, SCENARIO_MAX_CYCLES) > 0;
  prefs.end();
  return countOk && remainingOk;
}

bool skmCancelCycleCapture() {
  Preferences prefs;
  if (!prefs.begin("skm_diag", false)) return false;
  const bool existed = prefs.isKey(CAPTURE_REMAINING_KEY);
  prefs.remove(CAPTURE_REMAINING_KEY);
  prefs.end();
  return existed;
}

uint8_t skmCycleCaptureRemaining() {
  Preferences prefs;
  if (!prefs.begin("skm_diag", true)) return 0;
  const uint8_t remaining =
      prefs.getUChar(CAPTURE_REMAINING_KEY, 0);
  prefs.end();
  return remaining;
}

bool skmCycleCaptureRequested() {
  return skmCycleCaptureRemaining() > 0;
}

bool skmPersistCycleLogIfRequested() {
  Preferences prefs;
  if (!prefs.begin("skm_diag", true)) return false;
  uint8_t remaining = prefs.getUChar(CAPTURE_REMAINING_KEY, 0);
  uint8_t cycleCount = prefs.getUChar(CAPTURE_COUNT_KEY, 0);
  prefs.end();
  if (remaining == 0) return false;

  if (!SPIFFS.begin(true)) return false;
  File file = SPIFFS.open(SCENARIO_LOG_PATH, FILE_APPEND);
  if (!file) return false;

  char header[64];
  const int headerLength = snprintf(
      header, sizeof(header),
      "\n--- Production wake #%u ---\n",
      static_cast<unsigned>(cycleCount + 1));
  const size_t safeHeaderLength =
      headerLength > 0
        ? (static_cast<size_t>(headerLength) < sizeof(header)
             ? static_cast<size_t>(headerLength)
             : sizeof(header) - 1)
        : 0;
  const size_t required =
      safeHeaderLength + retainedCycleLog.length + 96;
  const bool capacityReached =
      file.size() + required > SCENARIO_LOG_MAX_BYTES;

  bool saved = !capacityReached;
  if (saved && safeHeaderLength > 0) {
    saved = file.write(
                reinterpret_cast<const uint8_t *>(header),
                safeHeaderLength) == safeHeaderLength;
  }
  if (saved && retainedCycleLog.length > 0) {
    saved = file.write(
                reinterpret_cast<const uint8_t *>(retainedCycleLog.text),
                retainedCycleLog.length) == retainedCycleLog.length;
  }
  if (saved &&
      (retainedCycleLog.length == 0 ||
       retainedCycleLog.text[retainedCycleLog.length - 1] != '\n')) {
    saved = file.write(static_cast<uint8_t>('\n')) == 1;
  }

  if (saved) {
    ++cycleCount;
    --remaining;
  }

  const bool stop = remaining == 0 || capacityReached || !saved;
  if (saved && stop) {
    const char *stopMessage =
        remaining == 0
          ? "[capture stopped: fifty-wake limit reached]\n"
          : "[capture stopped: log capacity reached]\n";
    saved = file.print(stopMessage) == strlen(stopMessage);
  }
  file.flush();
  file.close();

  Preferences updatePrefs;
  if (!updatePrefs.begin("skm_diag", false)) return false;
  const bool countSaved =
      updatePrefs.putUChar(CAPTURE_COUNT_KEY, cycleCount) > 0;
  bool stateSaved = true;
  if (stop) {
    updatePrefs.remove(CAPTURE_REMAINING_KEY);
  } else {
    stateSaved =
        updatePrefs.putUChar(CAPTURE_REMAINING_KEY, remaining) > 0;
  }
  updatePrefs.end();
  return saved && countSaved && stateSaved;
}

String skmPersistedCycleLogSnapshot() {
  if (!SPIFFS.begin(true) || !SPIFFS.exists(SCENARIO_LOG_PATH)) {
    return String();
  }

  File file = SPIFFS.open(SCENARIO_LOG_PATH, FILE_READ);
  if (!file) return String();

  Preferences prefs;
  uint8_t cycleCount = 0;
  if (prefs.begin("skm_diag", true)) {
    cycleCount = prefs.getUChar(CAPTURE_COUNT_KEY, 0);
    prefs.end();
  }

  String text;
  const size_t fileSize = file.size();
  text.reserve(fileSize + 96);
  text += F("Captured production wakes: ");
  text += String(cycleCount);
  text += F(" / maximum ");
  text += String(SCENARIO_MAX_CYCLES);
  text += F("\n");

  char buffer[256];
  while (file.available()) {
    const size_t readLength = file.read(
        reinterpret_cast<uint8_t *>(buffer), sizeof(buffer));
    if (readLength == 0) break;
    text.concat(buffer, static_cast<unsigned int>(readLength));
  }
  file.close();
  return text;
}
bool skmServiceRequested() {
  pinMode(SKIMMERSENSE_SERVICE_PIN, INPUT_PULLUP);
  delay(5);
  return digitalRead(SKIMMERSENSE_SERVICE_PIN) == LOW;
}

const char *skmDiagStageName(SkmDiagStage stage) {
  switch (stage) {
    case SkmDiagStage::BOOT: return "BOOT";
    case SkmDiagStage::SENSORS: return "SENSORS";
    case SkmDiagStage::PLAN: return "PLAN";
    case SkmDiagStage::ZIGBEE_START: return "ZIGBEE_START";
    case SkmDiagStage::ZIGBEE_CONNECTED: return "ZIGBEE_CONNECTED";
    case SkmDiagStage::REPORTING: return "REPORTING";
    case SkmDiagStage::SLEEP: return "SLEEP";
    case SkmDiagStage::SERVICE: return "SERVICE";
    default: return "UNKNOWN";
  }
}

void skmDiagnosticsBoot() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const RtcDiagnostics prior = rtcDiagnostics;
  const bool priorValid = prior.magic == RTC_DIAG_MAGIC;
  if (reason != ESP_RST_DEEPSLEEP) appendResetEvent(reason, priorValid, prior);

  const uint32_t nextCycle = priorValid ? prior.cycleCounter + 1 : 1;
  rtcDiagnostics = RtcDiagnostics{};
  rtcDiagnostics.magic = RTC_DIAG_MAGIC;
  rtcDiagnostics.cycleCounter = nextCycle;
  rtcDiagnostics.stage = static_cast<uint8_t>(SkmDiagStage::BOOT);
}

void skmDiagSetStage(SkmDiagStage stage) {
  rtcDiagnostics.stage = static_cast<uint8_t>(stage);
}

void skmDiagSetSensors(bool lowClosed, bool highClosed) {
  rtcDiagnostics.stage = static_cast<uint8_t>(SkmDiagStage::SENSORS);
  rtcDiagnostics.lowClosed = lowClosed ? 1 : 0;
  rtcDiagnostics.highClosed = highClosed ? 1 : 0;
}

void skmDiagSetPlan(uint8_t state, uint8_t nextState, uint32_t sleepSeconds) {
  rtcDiagnostics.stage = static_cast<uint8_t>(SkmDiagStage::PLAN);
  rtcDiagnostics.state = state;
  rtcDiagnostics.nextState = nextState;
  rtcDiagnostics.sleepSeconds = sleepSeconds;
}

void skmDiagSetZigbeeAttempt() {
  rtcDiagnostics.stage = static_cast<uint8_t>(SkmDiagStage::ZIGBEE_START);
  rtcDiagnostics.zigbeeAttempted = 1;
  rtcDiagnostics.zigbeeConnectedKnown = 0;
  rtcDiagnostics.reportsKnown = 0;
}

void skmDiagSetZigbeeConnected(bool connected) {
  rtcDiagnostics.stage = connected
      ? static_cast<uint8_t>(SkmDiagStage::ZIGBEE_CONNECTED)
      : static_cast<uint8_t>(SkmDiagStage::ZIGBEE_START);
  rtcDiagnostics.zigbeeConnectedKnown = 1;
  rtcDiagnostics.zigbeeConnected = connected ? 1 : 0;
}

void skmDiagSetReportsResult(bool ok) {
  rtcDiagnostics.stage = static_cast<uint8_t>(SkmDiagStage::REPORTING);
  rtcDiagnostics.reportsKnown = 1;
  rtcDiagnostics.reportsOk = ok ? 1 : 0;
}

void skmDiagSetSleep(uint8_t nextState, uint32_t sleepSeconds) {
  rtcDiagnostics.stage = static_cast<uint8_t>(SkmDiagStage::SLEEP);
  rtcDiagnostics.nextState = nextState;
  rtcDiagnostics.sleepSeconds = sleepSeconds;
}

[[noreturn]] void skmRunServiceMode(const char *firmwareVersion,
                                    const char *firmwareFlavor,
                                    SkmServiceStatusProvider statusProvider) {
  skmDiagSetStage(SkmDiagStage::SERVICE);
  serviceSessionLog = "";

  // XIAO ESP32-C6 RF switch: GPIO3 LOW enables switch control and GPIO14 LOW
  // selects the onboard ceramic antenna. SERVICE mode never starts Zigbee.
  pinMode(WIFI_ENABLE, OUTPUT);
  digitalWrite(WIFI_ENABLE, LOW);
  delay(100);
  pinMode(WIFI_ANT_CONFIG, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, LOW);

  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFULL);
  char suffixText[7];
  snprintf(suffixText, sizeof(suffixText), "%06lX", static_cast<unsigned long>(suffix));
  const String apSsid = String("SkimmerSense-") + suffixText;
  const String apPassword = String("SKM-") + suffixText;

  WiFi.mode(WIFI_AP_STA);
  // SERVICE mode is temporary and usually USB-powered: keep Wi-Fi awake for
  // responsive diagnostics and reliable OTA transfers.
  WiFi.setSleep(false);
  const bool apOk = WiFi.softAP(apSsid.c_str(), apPassword.c_str());
  const IPAddress apIp = WiFi.softAPIP();

  bool homeConfigured = false;
  bool staConnected = false;
  bool mdnsOk = false;
  String homeSsid;
  IPAddress staIp;

#if SKIMMERSENSE_HAS_WIFI_SECRETS
  homeSsid = SKIMMERSENSE_WIFI_SSID;
  homeConfigured = homeSsid.length() > 0;
  if (homeConfigured) {
    Serial.printf("Connecting to home Wi-Fi: %s\n", homeSsid.c_str());
    WiFi.begin(SKIMMERSENSE_WIFI_SSID, SKIMMERSENSE_WIFI_PASSWORD);
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < HOME_WIFI_TIMEOUT_MS) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
    staConnected = WiFi.status() == WL_CONNECTED;
    if (staConnected) {
      staIp = WiFi.localIP();
      mdnsOk = MDNS.begin(MDNS_HOSTNAME);
      if (mdnsOk) MDNS.addService("http", "tcp", 80);
    }
  }
#endif

  Serial.println();
  Serial.println("========== SERVICE MODE ==========");
  Serial.println("SERVICE jumper: D6/GPIO16 -> GND");
  Serial.printf("Fallback AP : %s\n", apSsid.c_str());
  Serial.printf("AP password : %s\n", apPassword.c_str());
  Serial.printf("AP address  : http://%s/\n", apIp.toString().c_str());
  Serial.printf("SoftAP      : %s\n", apOk ? "started" : "FAILED");
  if (!homeConfigured) {
    Serial.println("Home Wi-Fi  : not configured");
  } else if (staConnected) {
    Serial.printf("Home Wi-Fi  : connected to %s\n", homeSsid.c_str());
    Serial.printf("LAN address : http://%s/\n", staIp.toString().c_str());
    Serial.printf("mDNS        : %s\n", mdnsOk ? "http://skimmersense.local/" : "FAILED");
  } else {
    Serial.printf("Home Wi-Fi  : connection timeout (%s)\n", homeSsid.c_str());
  }
  Serial.println("Zigbee and deep sleep are disabled while SERVICE jumper is active.");
  Serial.println("==================================");

  appendServiceSessionLine(String(F("SERVICE mode started")));
  appendServiceSessionLine(String(F("Fallback AP: ")) + apSsid +
                           (apOk ? F(" (started)") : F(" (FAILED)")));
  if (staConnected) {
    appendServiceSessionLine(String(F("Home Wi-Fi connected: ")) +
                             homeSsid + F(" / ") + staIp.toString());
    appendServiceSessionLine(String(F("mDNS: ")) +
                             (mdnsOk ? F("skimmersense.local") : F("FAILED")));
  } else if (homeConfigured) {
    appendServiceSessionLine(String(F("Home Wi-Fi connection timeout: ")) +
                             homeSsid);
  } else {
    appendServiceSessionLine(String(F("Home Wi-Fi not configured")));
  }

  WebServer server(80);
  bool otaFinishedOk = false;
  String otaFailure;

  auto makeHeader = [&]() {
    return pageHeader(firmwareVersion, firmwareFlavor,
                      apSsid, apPassword, apIp,
                      homeConfigured, homeSsid,
                      staConnected, staIp, mdnsOk);
  };

  server.on("/capture-next", HTTP_POST, [&]() {
    const bool armed = skmRequestNextCycleCapture();
    appendServiceSessionLine(
        armed ? String(F("Next 50 production wakes capture ARMED"))
              : String(F("FAILED to arm production scenario capture")));
    server.sendHeader("Location", "/", true);
    server.send(armed ? 303 : 500,
                "text/plain; charset=utf-8",
                armed ? "Capture armed" : "Unable to arm capture");
  });

  server.on("/capture-cancel", HTTP_POST, [&]() {
    const bool cancelled = skmCancelCycleCapture();
    appendServiceSessionLine(
        cancelled ? String(F("Fifty-wake capture cancelled"))
                  : String(F("Fifty-wake capture was already inactive")));
    server.sendHeader("Location", "/", true);
    server.send(303, "text/plain; charset=utf-8", "Capture cancelled");
  });

  server.on("/logs.txt", HTTP_GET, [&]() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", combinedLogText());
  });

  server.on("/logs-download", HTTP_GET, [&]() {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Disposition",
                      "attachment; filename=skimmersense-logs.txt");
    server.send(200, "text/plain; charset=utf-8", combinedLogText());
  });

  server.on("/logs", HTTP_GET, [&]() {
    String html = makeHeader();
    html += F("<h2>Wi-Fi logs</h2><div class='card'>"
              "<p>The last production cycle is retained without flash writes. "
              "This SERVICE session refreshes every two seconds.</p>"
              "<p><a href='/'>Back to diagnostics</a> | "
              "<a href='/logs-download'>Download logs</a></p>"
              "<pre id='log' style='white-space:pre-wrap;overflow-wrap:anywhere;"
              "background:#111;color:#d9f2d9;padding:12px;border-radius:8px;"
              "min-height:280px'>Loading...</pre></div>"
              "<script>async function r(){try{const x=await fetch('/logs.txt',"
              "{cache:'no-store'});document.getElementById('log').textContent="
              "await x.text();}catch(e){document.getElementById('log').textContent="
              "='Log refresh failed: '+e;}}r();setInterval(r,2000);</script>");
    html += pageFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/", HTTP_GET, [&]() {
    String html = makeHeader();
    if (!apOk) {
      html += F("<p class='warn'><strong>Fallback AP failed to start.</strong></p>");
    }
    if (statusProvider != nullptr) {
      html += F("<h2>Live hardware</h2>");
      html += statusProvider();
    }
    html += diagnosticsHtml();
    html += resetLogHtml();
    html += F("<h2>Production-cycle capture</h2><div class='card'>");
    if (skmCycleCaptureRequested()) {
      html += F("<p class='ok'><strong>ARMED:</strong> capturing successive "
                "production wakes until return to NORMAL (maximum eight). "
                "Remaining limit: ");
      html += String(skmCycleCaptureRemaining());
      html += F(".</p><form method='POST' action='/capture-cancel'>"
                "<button type='submit'>Cancel scenario capture</button></form>");
    } else {
      html += F("<p>Capture a complete multi-wake state-machine scenario across "
                "RESET without continuous flash writes.</p>"
                "<form method='POST' action='/capture-next'>"
                "<button type='submit'>Capture next 50 wakes</button></form>");
    }
    html += F("</div>");
    html += F("<h2>Logs</h2><div class='card'><p>"
              "<a href='/logs'>Open live Wi-Fi logs</a> | "
              "<a href='/logs-download'>Download logs</a></p>"
              "<p>Includes the retained last production cycle, persistent scenario and current "
              "SERVICE session.</p></div>");
    html += F("<h2>OTA firmware update</h2><div class='card'><p>Upload a PlatformIO <code>firmware.bin</code>. The inactive OTA application slot is written; Zigbee storage is not intentionally erased.</p>");
    html += F("<p class='warn'><strong>After a successful upload:</strong> remove the D6-GND SERVICE jumper before rebooting if you want normal production mode.</p>");
    html += F("<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' accept='.bin' required> <input type='submit' value='Upload firmware'></form></div>");
    html += F("<h2>Restart</h2><div class='card'><form method='POST' action='/reboot'><button type='submit'>Reboot SkimmerSense</button></form></div>");
    html += pageFooter();
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/reboot", HTTP_POST, [&]() {
    appendServiceSessionLine(String(F("Software reboot requested from web page")));
    server.send(200, "text/html; charset=utf-8", "<html><body><h2>Rebooting...</h2></body></html>");
    delay(300);
    ESP.restart();
  });

  server.on(
      "/update", HTTP_POST,
      [&]() {
        String html = makeHeader();
        if (otaFinishedOk && !Update.hasError()) {
          html += F("<h2>OTA upload successful</h2><p>The new firmware is stored in the alternate OTA slot.</p><p class='warn'><strong>Remove the D6-GND SERVICE jumper now</strong>, then reboot to return to normal operation.</p><form method='POST' action='/reboot'><button type='submit'>Reboot into new firmware</button></form>");
        } else {
          html += F("<h2>OTA upload failed</h2><p>");
          html += otaFailure.length() ? otaFailure : String("Update library reported an error.");
          html += F("</p><p><a href='/'>Back to service page</a></p>");
        }
        html += pageFooter();
        server.sendHeader("Connection", "close");
        server.send(200, "text/html; charset=utf-8", html);
      },
      [&]() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          otaFinishedOk = false;
          otaFailure = "";
          Serial.printf("OTA start: %s\n", upload.filename.c_str());
          appendServiceSessionLine(String(F("OTA start: ")) + upload.filename);
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaFailure = "Unable to open the OTA application partition.";
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (!Update.hasError()) {
            const size_t written = Update.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize) {
              otaFailure = "Flash write failed during OTA upload.";
              Update.printError(Serial);
            }
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (!Update.hasError() && Update.end(true)) {
            otaFinishedOk = true;
            Serial.printf("OTA complete: %u bytes\n", static_cast<unsigned>(upload.totalSize));
            appendServiceSessionLine(
                String(F("OTA complete: ")) + String(upload.totalSize) + F(" bytes"));
          } else {
            if (!otaFailure.length()) otaFailure = "OTA finalization failed.";
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          otaFailure = "Upload aborted by client.";
          Serial.println("OTA upload aborted");
          appendServiceSessionLine(String(F("OTA upload aborted by client")));
        }
      });

  server.onNotFound([&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("Service HTTP server started.");
  appendServiceSessionLine(String(F("Service HTTP server started")));

  while (true) {
    server.handleClient();
    delay(2);
  }
}
