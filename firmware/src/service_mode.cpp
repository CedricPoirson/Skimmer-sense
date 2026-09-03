#include "service_mode.h"

#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace {

static constexpr uint32_t RTC_DIAG_MAGIC = 0x534B4431UL;   // "SKD1"
static constexpr uint32_t RESET_LOG_MAGIC = 0x534B4C31UL;  // "SKL1"
static constexpr uint8_t RESET_LOG_CAPACITY = 8;

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
  uint8_t head = 0;  // next insertion slot
  uint8_t reserved[2] = {0, 0};
  PersistedResetEvent events[RESET_LOG_CAPACITY];
};

RTC_DATA_ATTR RtcDiagnostics rtcDiagnostics;

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
  if (!prefs.begin("skm_diag", true)) {
    return log;
  }

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
  if (!prefs.begin("skm_diag", false)) {
    return;
  }
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
  if (log.count < RESET_LOG_CAPACITY) {
    ++log.count;
  }

  saveResetLog(log);
}

String yesNoUnknown(uint8_t known, uint8_t value) {
  if (!known) return "unknown";
  return value ? "yes" : "no";
}

String resetLogHtml() {
  const PersistedResetLog log = loadResetLog();
  String html;
  html.reserve(2500);
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

    html += F("<tr><td>");
    html += String(event.sequence);
    html += F("</td><td>");
    html += resetReasonName(static_cast<esp_reset_reason_t>(event.resetReason));
    html += F("</td>");

    if (!event.priorRtcValid) {
      html += F("<td colspan='7'>No retained RTC snapshot (power loss/cold boot possible)</td></tr>");
      continue;
    }

    html += F("<td>");
    html += skmDiagStageName(static_cast<SkmDiagStage>(event.stage));
    html += F("</td><td>");
    html += String(event.state);
    html += F("</td><td>");
    html += String(event.nextState);
    html += F("</td><td>");
    html += event.lowClosed ? F("CLOSED/") : F("OPEN/");
    html += event.highClosed ? F("CLOSED") : F("OPEN");
    html += F("</td><td>");
    html += yesNoUnknown(event.zigbeeConnectedKnown, event.zigbeeConnected);
    html += F("</td><td>");
    html += yesNoUnknown(event.reportsKnown, event.reportsOk);
    html += F("</td><td>");
    html += String(event.sleepSeconds);
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
  html += F(" (");
  html += String(static_cast<int>(esp_reset_reason()));
  html += F(")</td></tr>");
  html += F("<tr><th>Retained cycle counter</th><td>");
  html += String(rtcDiagnostics.cycleCounter);
  html += F("</td></tr><tr><th>Last stage</th><td>");
  html += skmDiagStageName(static_cast<SkmDiagStage>(rtcDiagnostics.stage));
  html += F("</td></tr><tr><th>State / next</th><td>");
  html += String(rtcDiagnostics.state);
  html += F(" / ");
  html += String(rtcDiagnostics.nextState);
  html += F("</td></tr><tr><th>LOW / HIGH</th><td>");
  html += rtcDiagnostics.lowClosed ? F("CLOSED / ") : F("OPEN / ");
  html += rtcDiagnostics.highClosed ? F("CLOSED") : F("OPEN");
  html += F("</td></tr><tr><th>Zigbee attempted</th><td>");
  html += rtcDiagnostics.zigbeeAttempted ? F("yes") : F("no");
  html += F("</td></tr><tr><th>Zigbee connected</th><td>");
  html += yesNoUnknown(rtcDiagnostics.zigbeeConnectedKnown,
                       rtcDiagnostics.zigbeeConnected);
  html += F("</td></tr><tr><th>Reports OK</th><td>");
  html += yesNoUnknown(rtcDiagnostics.reportsKnown, rtcDiagnostics.reportsOk);
  html += F("</td></tr><tr><th>Planned sleep</th><td>");
  html += String(rtcDiagnostics.sleepSeconds);
  html += F(" s</td></tr></table>");
  return html;
}

String pageHeader(const char *firmwareVersion,
                  const char *firmwareFlavor,
                  const String &ssid,
                  const String &password,
                  const IPAddress &ip) {
  String html;
  html.reserve(2500);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>SkimmerSense Service</title><style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;max-width:900px;margin:auto;padding:18px;background:#f4f6f8;color:#17202a}h1{margin-bottom:4px}h2{margin-top:28px}table{border-collapse:collapse;width:100%;background:white}th,td{border:1px solid #d8dde3;padding:7px;text-align:left}th{background:#eef2f5}code{background:#e8edf1;padding:2px 5px;border-radius:4px}.card{background:white;padding:14px;border-radius:10px;margin:12px 0}.warn{background:#fff4d6;padding:12px;border-radius:8px}button,input[type=submit]{font-size:16px;padding:9px 14px}</style></head><body>");
  html += F("<h1>SkimmerSense Service</h1><p>Firmware <strong>");
  html += firmwareVersion;
  html += F("</strong> — ");
  html += firmwareFlavor;
  html += F("</p><div class='card'><strong>Maintenance AP</strong><br>SSID: <code>");
  html += ssid;
  html += F("</code><br>Password: <code>");
  html += password;
  html += F("</code><br>Address: <code>http://");
  html += ip.toString();
  html += F("/</code></div>");
  return html;
}

String pageFooter() {
  return F("</body></html>");
}

}  // namespace

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

  // Deep-sleep wakes are the normal operating cycle and must not wear NVS.
  // Any other reset (power-on, panic, watchdog, brownout, manual reset, OTA)
  // is rare enough to keep as a persistent forensic event.
  if (reason != ESP_RST_DEEPSLEEP) {
    appendResetEvent(reason, priorValid, prior);
  }

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

  // XIAO ESP32-C6 RF switch: GPIO3 LOW enables switch control and GPIO14 LOW
  // selects the onboard ceramic antenna. Service mode never starts Zigbee.
  pinMode(WIFI_ENABLE, OUTPUT);
  digitalWrite(WIFI_ENABLE, LOW);
  delay(100);
  pinMode(WIFI_ANT_CONFIG, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, LOW);

  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFULL);
  char suffixText[7];
  snprintf(suffixText, sizeof(suffixText), "%06lX",
           static_cast<unsigned long>(suffix));

  const String ssid = String("SkimmerSense-") + suffixText;
  const String password = String("SKM-") + suffixText;

  WiFi.mode(WIFI_AP);
  const bool apOk = WiFi.softAP(ssid.c_str(), password.c_str());
  const IPAddress ip = WiFi.softAPIP();

  Serial.println();
  Serial.println("========== SERVICE MODE ==========");
  Serial.printf("SERVICE jumper: D6/GPIO16 -> GND\n");
  Serial.printf("Wi-Fi AP: %s\n", ssid.c_str());
  Serial.printf("Password : %s\n", password.c_str());
  Serial.printf("Address  : http://%s/\n", ip.toString().c_str());
  Serial.printf("SoftAP   : %s\n", apOk ? "started" : "FAILED");
  Serial.println("Zigbee and deep sleep are disabled while SERVICE jumper is active.");
  Serial.println("==================================");

  WebServer server(80);
  bool otaFinishedOk = false;
  String otaFailure;

  server.on("/", HTTP_GET, [&]() {
    String html = pageHeader(firmwareVersion, firmwareFlavor, ssid, password, ip);

    if (!apOk) {
      html += F("<p class='warn'><strong>Wi-Fi AP failed to start.</strong> Reboot and inspect the serial log.</p>");
    }

    if (statusProvider != nullptr) {
      html += F("<h2>Live hardware</h2>");
      html += statusProvider();
    }

    html += diagnosticsHtml();
    html += resetLogHtml();

    html += F("<h2>OTA firmware update</h2><div class='card'><p>The Zigbee partition table already contains two OTA application slots. Uploading a firmware image updates only the application slot; Zigbee pairing/storage is not erased.</p>");
    html += F("<p class='warn'><strong>Before rebooting after a successful upload:</strong> remove the D6-GND SERVICE jumper if you want normal production mode.</p>");
    html += F("<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' accept='.bin' required> <input type='submit' value='Upload firmware'></form></div>");

    html += F("<h2>Restart</h2><div class='card'><form method='POST' action='/reboot'><button type='submit'>Reboot SkimmerSense</button></form></div>");
    html += pageFooter();
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/reboot", HTTP_POST, [&]() {
    server.send(200, "text/html; charset=utf-8",
                "<html><body><h2>Rebooting...</h2></body></html>");
    delay(300);
    ESP.restart();
  });

  server.on(
      "/update", HTTP_POST,
      [&]() {
        String html = pageHeader(firmwareVersion, firmwareFlavor, ssid, password, ip);
        if (otaFinishedOk && !Update.hasError()) {
          html += F("<h2>OTA upload successful</h2><p>The new firmware is stored in the alternate OTA slot.</p><p class='warn'><strong>Remove the D6-GND SERVICE jumper now</strong>, then use the reboot button below to return to normal operation.</p><form method='POST' action='/reboot'><button type='submit'>Reboot into new firmware</button></form>");
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
            Serial.printf("OTA complete: %u bytes\n",
                          static_cast<unsigned>(upload.totalSize));
          } else {
            if (!otaFailure.length()) {
              otaFailure = "OTA finalization failed.";
            }
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          otaFailure = "Upload aborted by client.";
          Serial.println("OTA upload aborted");
        }
      });

  server.onNotFound([&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("Service HTTP server started.");

  while (true) {
    server.handleClient();
    delay(2);
  }
}
