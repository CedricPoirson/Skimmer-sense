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
#include "skm_radio.h"

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

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 32);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '\\': escaped += F("\\\\"); break;
      case '"': escaped += F("\\\""); break;
      case '\n': escaped += F("\\n"); break;
      case '\r': escaped += F("\\r"); break;
      case '\t': escaped += F("\\t"); break;
      default:
        if (static_cast<uint8_t>(c) >= 0x20) escaped += c;
        break;
    }
  }
  return escaped;
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
  html.reserve(10500);
  html += F(R"HTML(<!doctype html><html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#071b2b"><title>SkimmerSense Service</title>
<style>
:root{--bg:#06131f;--panel:#0d2233;--panel2:#102b40;--line:#21445b;--text:#effaff;--muted:#8fb0c4;--cyan:#28c5e5;--blue:#2794ff;--green:#3ddc97;--amber:#ffbf57;--red:#ff6b79;--shadow:0 18px 50px rgba(0,0,0,.25)}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:radial-gradient(circle at 85% 0,#103a55 0,transparent 33%),var(--bg);color:var(--text);min-height:100vh}
a{color:var(--cyan);text-decoration:none}a:hover{text-decoration:underline}.shell{width:min(1120px,calc(100% - 28px));margin:auto}.top{position:sticky;top:0;z-index:10;background:rgba(6,19,31,.82);backdrop-filter:blur(18px);border-bottom:1px solid rgba(255,255,255,.08)}
.topin{height:64px;display:flex;align-items:center;justify-content:space-between}.brand{display:flex;align-items:center;gap:11px;font-weight:750}.logo{width:34px;height:34px;border-radius:11px;background:linear-gradient(135deg,var(--cyan),var(--blue));display:grid;place-items:center;box-shadow:0 0 24px rgba(40,197,229,.35)}.logo:after{content:"~";font-size:25px;font-weight:800}
.live{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:13px}.dot{width:9px;height:9px;border-radius:50%;background:var(--green);box-shadow:0 0 0 5px rgba(61,220,151,.12);animation:pulse 2s infinite}@keyframes pulse{50%{opacity:.45}}
.hero{padding:34px 0 22px}.hero h1{margin:0;font-size:clamp(29px,5vw,45px);letter-spacing:-.04em}.hero p{color:var(--muted);margin:9px 0 0}.badge{display:inline-flex;padding:5px 10px;border-radius:999px;background:rgba(40,197,229,.12);color:var(--cyan);border:1px solid rgba(40,197,229,.25);font-size:12px;font-weight:700}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}.card{grid-column:span 6;background:linear-gradient(145deg,rgba(16,43,64,.95),rgba(11,31,47,.95));border:1px solid rgba(255,255,255,.08);border-radius:18px;padding:18px;box-shadow:var(--shadow)}.card.full{grid-column:1/-1}.card.third{grid-column:span 4}.card h2,.card h3{margin:0 0 13px}.eyebrow{text-transform:uppercase;letter-spacing:.12em;color:var(--muted);font-size:11px;font-weight:800}.value{font-size:22px;font-weight:760;margin-top:6px}.sub{color:var(--muted);font-size:13px;line-height:1.5}.status{display:inline-flex;align-items:center;gap:7px;color:var(--green);font-weight:700}.status:before{content:"";width:8px;height:8px;background:currentColor;border-radius:50%}
section{margin:0 0 20px}.section-title{display:flex;align-items:end;justify-content:space-between;margin:28px 2px 12px}.section-title h2{margin:0}.section-title small{color:var(--muted)}
table{border-collapse:separate;border-spacing:0;width:100%;background:transparent;overflow:hidden}th,td{padding:10px 12px;text-align:left;border-bottom:1px solid var(--line);font-size:14px}th{color:var(--muted);font-weight:650}tr:last-child th,tr:last-child td{border-bottom:0}code{background:#071724;padding:3px 6px;border-radius:6px;color:#bceeff;overflow-wrap:anywhere}
button,.button,input[type=submit]{appearance:none;border:0;border-radius:11px;padding:11px 15px;background:linear-gradient(135deg,var(--cyan),var(--blue));color:#04141f;font:inherit;font-weight:760;cursor:pointer;transition:.18s transform,.18s opacity;display:inline-flex;align-items:center;justify-content:center;gap:7px}button:hover,.button:hover{transform:translateY(-1px);text-decoration:none}button:disabled{opacity:.45;cursor:wait;transform:none}.secondary{background:#183a51;color:var(--text);border:1px solid var(--line)}.danger{background:rgba(255,107,121,.15);color:#ffadb6;border:1px solid rgba(255,107,121,.35)}
.actions{display:flex;gap:9px;flex-wrap:wrap}.warn,.ok,.info{padding:12px 14px;border-radius:12px;margin:12px 0}.warn{background:rgba(255,191,87,.12);border:1px solid rgba(255,191,87,.28);color:#ffd995}.ok{background:rgba(61,220,151,.11);border:1px solid rgba(61,220,151,.25);color:#9cf0c8}.info{background:rgba(39,148,255,.11);border:1px solid rgba(39,148,255,.25)}
.metricbar{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin:12px 0}.metric{background:rgba(2,13,22,.35);padding:12px;border-radius:12px}.metric b{display:block;font-size:19px;margin-top:4px}
pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#031018;color:#b8fbd7;padding:16px;border-radius:14px;min-height:310px;max-height:68vh;overflow:auto;border:1px solid #173b4b;font:12px/1.55 ui-monospace,SFMono-Regular,Menlo,monospace}
.filebox{border:1px dashed #3b6680;border-radius:13px;padding:16px;background:rgba(0,0,0,.12)}input[type=file]{max-width:100%;color:var(--muted)}progress{width:100%;height:12px;accent-color:var(--cyan);margin-top:12px}.hidden{display:none!important}.skeleton{height:95px;border-radius:12px;background:linear-gradient(90deg,#112b3d 25%,#193c51 50%,#112b3d 75%);background-size:200% 100%;animation:shine 1.5s infinite}@keyframes shine{to{background-position:-200% 0}}
.toast{position:fixed;right:18px;bottom:18px;z-index:30;max-width:min(390px,calc(100% - 36px));padding:13px 16px;border-radius:12px;background:#17394d;border:1px solid #2c5971;box-shadow:var(--shadow);transform:translateY(120px);opacity:0;transition:.25s}.toast.show{transform:none;opacity:1}.toast.error{border-color:var(--red);color:#ffc0c6}
footer{color:var(--muted);font-size:12px;padding:20px 0 35px;text-align:center}
@media(max-width:760px){.card,.card.third{grid-column:1/-1}.metricbar{grid-template-columns:1fr}.topin{height:58px}.hero{padding-top:25px}.section-title{align-items:start;flex-direction:column;gap:4px}table{display:block;overflow-x:auto}}
</style></head><body><header class="top"><div class="shell topin"><div class="brand"><span class="logo"></span>SkimmerSense</div><div class="live"><span class="dot"></span><span id="liveLabel">SERVICE en ligne</span></div></div></header><main class="shell">
<div class="hero"><span class="badge">MODE MAINTENANCE</span><h1>Tableau de bord</h1><p>Diagnostic, historique et mise à jour locale de votre capteur de piscine.</p></div>
<div class="grid">)HTML");
  html += F("<div class='card third'><div class='eyebrow'>Firmware</div><div class='value'>");
  html += firmwareVersion; html += F("</div><div class='sub'>"); html += firmwareFlavor; html += F("</div></div>");
  html += F("<div class='card third'><div class='eyebrow'>Wi-Fi maison</div>");
  if (staConnected) {
    html += F("<div class='value status'>Connecté</div><div class='sub'>");
    html += homeSsid; html += F(" · "); html += staIp.toString(); html += F("<br>");
    html += mdnsOk ? F("skimmersense.local") : F("mDNS indisponible");
    html += F("</div>");
  } else {
    html += F("<div class='value' style='color:var(--amber)'>Non connecté</div><div class='sub'>");
    html += homeConfigured ? homeSsid : String(F("Non configuré"));
    html += F("</div>");
  }
  html += F("</div><div class='card third'><div class='eyebrow'>Point d’accès secours</div><div class='value'>");
  html += apSsid; html += F("</div><div class='sub'>"); html += apIp.toString();
  html += F(" · mot de passe <code>"); html += apPassword; html += F("</code></div></div></div>");
  return html;
}

String pageFooter() {
  return F(R"HTML(<footer>SkimmerSense · interface locale autonome</footer></main>
<div id="toast" class="toast"></div>
<script>
const $=id=>document.getElementById(id);
function toast(message,error=false){const t=$('toast');if(!t)return;t.textContent=message;t.className='toast show'+(error?' error':'');clearTimeout(window._toastTimer);window._toastTimer=setTimeout(()=>t.className='toast',4200)}
function formatUptime(seconds){seconds=Number(seconds)||0;const d=Math.floor(seconds/86400),h=Math.floor(seconds%86400/3600),m=Math.floor(seconds%3600/60);return(d?d+' j ':'')+(h?h+' h ':'')+m+' min'}
async function postAction(url,success){try{const r=await fetch(url+'?ajax=1',{method:'POST',cache:'no-store'});let d={};try{d=await r.json()}catch(e){}if(!r.ok||d.ok===false)throw new Error(d.message||'Action refusée');toast(d.message||success);if(window.refreshDashboard)setTimeout(window.refreshDashboard,350)}catch(e){toast(e.message||String(e),true)}}
</script></body></html>)HTML");
}

}  // namespace}  // namespace

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

  // Wi-Fi and Zigbee share the same 2.4 GHz RF path on the XIAO.
  skmSelectRadioAntenna();

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
  Serial.printf("RF antenna : %s\n", skmRadioAntennaName());
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
  appendServiceSessionLine(String(F("RF antenna: ")) + skmRadioAntennaName());
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
    if (server.hasArg("ajax")) {
      server.send(armed ? 200 : 500, "application/json; charset=utf-8",
                  armed
                    ? "{\"ok\":true,\"message\":\"Capture de 50 réveils activée\"}"
                    : "{\"ok\":false,\"message\":\"Impossible d’activer la capture\"}");
    } else {
      server.sendHeader("Location", "/", true);
      server.send(armed ? 303 : 500, "text/plain; charset=utf-8",
                  armed ? "Capture armed" : "Unable to arm capture");
    }
  });

  server.on("/capture-cancel", HTTP_POST, [&]() {
    const bool cancelled = skmCancelCycleCapture();
    appendServiceSessionLine(
        cancelled ? String(F("Fifty-wake capture cancelled"))
                  : String(F("Fifty-wake capture was already inactive")));
    if (server.hasArg("ajax")) {
      server.send(200, "application/json; charset=utf-8",
                  cancelled
                    ? "{\"ok\":true,\"message\":\"Capture annulée\"}"
                    : "{\"ok\":true,\"message\":\"La capture était déjà inactive\"}");
    } else {
      server.sendHeader("Location", "/", true);
      server.send(303, "text/plain; charset=utf-8", "Capture cancelled");
    }
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
    html += F(R"HTML(
<section><div class="section-title"><div><h2>Journaux en direct</h2><small>Mise à jour automatique toutes les deux secondes</small></div><div class="actions"><a class="button secondary" href="/">Retour</a><a class="button" href="/logs-download">Télécharger</a></div></div>
<div class="card full"><div class="live" style="margin-bottom:10px"><span class="dot"></span><span id="logState">Connexion aux journaux…</span></div><pre id="log">Chargement…</pre></div></section>
<script>
let firstLog=true;
async function refreshLogs(){try{const r=await fetch('/logs.txt',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const text=await r.text(),box=$('log');const nearBottom=box.scrollHeight-box.scrollTop-box.clientHeight<70;box.textContent=text;$('logState').textContent='Dernière mise à jour : '+new Date().toLocaleTimeString();if(firstLog||nearBottom)box.scrollTop=box.scrollHeight;firstLog=false}catch(e){$('logState').textContent='Connexion interrompue';toast('Actualisation des logs impossible',true)}}
refreshLogs();setInterval(refreshLogs,2000);
</script>)HTML");
    html += pageFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/api/status", HTTP_GET, [&]() {
    const String hardware =
        statusProvider != nullptr
          ? statusProvider()
          : String(F("<p class='warn'>Mesures matérielles indisponibles.</p>"));
    const String diagnostics = diagnosticsHtml();
    const String resets = resetLogHtml();
    String json;
    json.reserve(hardware.length() + diagnostics.length() + resets.length() + 320);
    json += F("{\"hardware\":\""); json += jsonEscape(hardware);
    json += F("\",\"diagnostics\":\""); json += jsonEscape(diagnostics);
    json += F("\",\"resets\":\""); json += jsonEscape(resets);
    json += F("\",\"capture_active\":");
    json += skmCycleCaptureRequested() ? F("true") : F("false");
    json += F(",\"capture_remaining\":");
    json += String(skmCycleCaptureRemaining());
    json += F(",\"heap\":"); json += String(ESP.getFreeHeap());
    json += F(",\"uptime\":"); json += String(millis() / 1000UL);
    json += F(",\"wifi_connected\":");
    json += WiFi.status() == WL_CONNECTED ? F("true") : F("false");
    json += F(",\"wifi_rssi\":");
    json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String(0);
    json += F("}");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", json);
  });

  server.on("/", HTTP_GET, [&]() {
    String html = makeHeader();
    if (!apOk) {
      html += F("<p class='warn'><strong>Le point d’accès de secours n’a pas démarré.</strong></p>");
    }
    html += F(R"HTML(
<section><div class="section-title"><div><h2>État en direct</h2><small>Actualisation automatique · aucun rechargement nécessaire</small></div></div>
<div class="metricbar"><div class="metric"><span class="eyebrow">Wi-Fi RSSI</span><b id="rssi">—</b></div><div class="metric"><span class="eyebrow">Mémoire libre</span><b id="heap">—</b></div><div class="metric"><span class="eyebrow">Session SERVICE</span><b id="uptime">—</b></div></div>
<div id="hardware" class="card full"><div class="skeleton"></div></div></section>
<section><div class="section-title"><div><h2>Diagnostic</h2><small>État RTC et dernier parcours du firmware</small></div></div><div id="diagnostics" class="card full"><div class="skeleton"></div></div></section>
<section><div class="section-title"><div><h2>Historique des redémarrages</h2><small>Conservé pour identifier watchdog, brownout et crash</small></div></div><div id="resets" class="card full"><div class="skeleton"></div></div></section>
<section><div class="section-title"><div><h2>Capture de scénario</h2><small>Jusqu’à 50 réveils de production</small></div></div><div id="capture" class="card full"><div class="skeleton"></div></div></section>
<section><div class="section-title"><div><h2>Journaux</h2><small>Cycles Zigbee, décisions et mesures</small></div></div><div class="card full"><div class="actions"><a class="button" href="/logs">Ouvrir les logs en direct</a><a class="button secondary" href="/logs-download">Télécharger</a></div></div></section>
<section><div class="section-title"><div><h2>Mise à jour OTA</h2><small>Écriture sécurisée dans le slot applicatif inactif</small></div></div><div class="card full">
<div class="warn"><strong>Après une mise à jour réussie :</strong> retire le jumper SERVICE D6–GND avant de redémarrer pour revenir en production.</div>
<form id="otaForm" class="filebox"><input id="firmwareFile" type="file" name="firmware" accept=".bin" required><div class="actions" style="margin-top:13px"><button id="otaButton" type="submit">Installer le firmware</button></div><progress id="otaProgress" class="hidden" value="0" max="100"></progress><div id="otaText" class="sub" style="margin-top:8px"></div></form>
</div></section>
<section><div class="section-title"><div><h2>Redémarrage</h2><small>Le mode sélectionné dépend de la position du jumper</small></div></div><div class="card full"><button class="danger" onclick="rebootDevice()">Redémarrer SkimmerSense</button></div></section>
<script>
let statusBusy=false;
window.refreshDashboard=async function(){if(statusBusy)return;statusBusy=true;try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();$('hardware').innerHTML=d.hardware;$('diagnostics').innerHTML=d.diagnostics;$('resets').innerHTML=d.resets;$('rssi').textContent=d.wifi_connected?d.wifi_rssi+' dBm':'hors ligne';$('heap').textContent=Math.round(d.heap/1024)+' Ko';$('uptime').textContent=formatUptime(d.uptime);$('liveLabel').textContent='Mis à jour à '+new Date().toLocaleTimeString();$('capture').innerHTML=d.capture_active?'<div class="ok"><strong>Capture active</strong> · '+d.capture_remaining+' réveil(s) restant(s)</div><button class="secondary" onclick="postAction(\'/capture-cancel\',\'Capture annulée\')">Annuler la capture</button>':'<p class="sub">Enregistre les prochains réveils de production afin de reconstituer un scénario complet, même après plusieurs passages en veille.</p><button onclick="postAction(\'/capture-next\',\'Capture activée\')">Capturer les 50 prochains réveils</button>';}catch(e){$('liveLabel').textContent='Connexion interrompue';toast('Tableau de bord momentanément inaccessible',true)}finally{statusBusy=false}}
function uploadFirmware(event){event.preventDefault();const file=$('firmwareFile').files[0];if(!file){toast('Sélectionne un fichier firmware.bin',true);return}if(!file.name.toLowerCase().endsWith('.bin')){toast('Le fichier doit être au format .bin',true);return}const form=new FormData();form.append('firmware',file);const xhr=new XMLHttpRequest(),bar=$('otaProgress'),button=$('otaButton'),label=$('otaText');button.disabled=true;bar.classList.remove('hidden');bar.value=0;label.textContent='Préparation de l’envoi…';xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded*100/e.total);bar.value=p;label.textContent='Téléversement : '+p+' %'}};xhr.onload=()=>{button.disabled=false;let d={};try{d=JSON.parse(xhr.responseText)}catch(e){}if(xhr.status>=200&&xhr.status<300&&d.ok!==false){bar.value=100;label.innerHTML='<span class="ok" style="display:block">Firmware installé. Retire maintenant le jumper SERVICE, puis redémarre.</span><button type="button" class="danger" onclick="rebootDevice()">Redémarrer</button>';toast(d.message||'Mise à jour OTA réussie')}else{label.textContent=d.message||'Échec de la mise à jour OTA';toast(label.textContent,true)}};xhr.onerror=()=>{button.disabled=false;label.textContent='Connexion interrompue pendant l’OTA';toast(label.textContent,true)};xhr.open('POST','/update?ajax=1');xhr.send(form)}
async function rebootDevice(){toast('Redémarrage en cours…');try{await fetch('/reboot?ajax=1',{method:'POST'})}catch(e){}setTimeout(()=>{$('liveLabel').textContent='Redémarrage…'},300)}
$('otaForm').addEventListener('submit',uploadFirmware);refreshDashboard();setInterval(refreshDashboard,5000);
</script>)HTML");
    html += pageFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/reboot", HTTP_POST, [&]() {
    appendServiceSessionLine(String(F("Software reboot requested from web page")));
    if (server.hasArg("ajax")) {
      server.send(200, "application/json; charset=utf-8",
                  "{\"ok\":true,\"message\":\"Redémarrage en cours\"}");
    } else {
      server.send(200, "text/html; charset=utf-8",
                  "<html><body><h2>Redémarrage…</h2></body></html>");
    }
    delay(400);
    ESP.restart();
  });

  server.on(
      "/update", HTTP_POST,
      [&]() {
        if (server.hasArg("ajax")) {
          String json;
          if (otaFinishedOk && !Update.hasError()) {
            json = F("{\"ok\":true,\"message\":\"Mise à jour OTA terminée\"}");
            server.sendHeader("Connection", "close");
            server.send(200, "application/json; charset=utf-8", json);
          } else {
            json = F("{\"ok\":false,\"message\":\"");
            json += jsonEscape(otaFailure.length()
                                 ? otaFailure
                                 : String(F("La bibliothèque OTA a signalé une erreur.")));
            json += F("\"}");
            server.sendHeader("Connection", "close");
            server.send(500, "application/json; charset=utf-8", json);
          }
        } else {
          String html = makeHeader();
          if (otaFinishedOk && !Update.hasError()) {
            html += F("<section><div class='card full'><h2>Mise à jour OTA réussie</h2><div class='warn'><strong>Retire le jumper SERVICE D6–GND</strong>, puis redémarre pour revenir en production.</div><form method='POST' action='/reboot'><button type='submit'>Redémarrer</button></form></div></section>");
          } else {
            html += F("<section><div class='card full'><h2>Échec de la mise à jour OTA</h2><p>");
            html += otaFailure.length() ? otaFailure : String(F("La bibliothèque OTA a signalé une erreur."));
            html += F("</p><a class='button secondary' href='/'>Retour</a></div></section>");
          }
          html += pageFooter();
          server.sendHeader("Connection", "close");
          server.send(200, "text/html; charset=utf-8", html);
        }
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
