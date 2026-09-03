#include "service_mode.h"

// Keep the validated deep-sleep firmware source bit-for-bit unchanged.
// It is compiled inside this translation unit under private entry-point names;
// the small wrapper below decides whether to run SERVICE mode or the normal
// SkimmerSense setup/loop.
#define setup skmNormalSetup
#define loop skmNormalLoop
#include "sleep_main.cpp"
#undef setup
#undef loop

namespace {

String buildServiceHardwareHtml() {
  SensorSnapshot snapshot = readBaseSensorSnapshot();
  readTemperatureIntoSnapshot(snapshot);

  String html;
  html.reserve(1800);
  html += F("<table>");
  html += F("<tr><th>Retained level state</th><td>");
  if (rtcMagic == RTC_MAGIC &&
      rtcStateRaw <= static_cast<uint8_t>(LevelState::WAIT_HIGH)) {
    html += stateName(static_cast<LevelState>(rtcStateRaw));
  } else {
    html += F("not valid / cold boot");
  }
  html += F("</td></tr>");

  html += F("<tr><th>LOW float</th><td>");
  html += snapshot.lowClosed ? F("CLOSED (ON)") : F("OPEN (OFF)");
  html += F("</td></tr><tr><th>HIGH float</th><td>");
  html += snapshot.highClosed ? F("CLOSED (ON)") : F("OPEN (OFF)");
  html += F("</td></tr>");

  html += F("<tr><th>Water temperature</th><td>");
  if (snapshot.temperatureValid) {
    html += String(snapshot.waterTemperatureC, 2);
    html += F(" &deg;C");
  } else {
    html += F("invalid / sensor unavailable");
  }
  html += F("</td></tr>");

  html += F("<tr><th>MAX17048</th><td>");
  if (snapshot.batteryValid) {
    html += String(snapshot.batteryVoltage, 3);
    html += F(" V / raw SOC ");
    html += String(snapshot.batterySocRaw, 1);
    html += F(" % / rounded ");
    html += String(snapshot.batteryPercent);
    html += F(" % / INT ");
    html += snapshot.maxIntLow ? F("LOW") : F("HIGH");
  } else {
    html += F("unavailable");
  }
  html += F("</td></tr>");

  html += F("<tr><th>Adaptive NORMAL cache</th><td>");
  if (rtcNormalSleepValid) {
    html += String(static_cast<unsigned long long>(rtcNormalSleepSeconds));
    html += F(" s from ");
    html += String(rtcLastWaterTemperatureC, 2);
    html += F(" &deg;C");
  } else {
    html += F("not valid");
  }
  html += F("</td></tr>");

  html += F("<tr><th>Free heap</th><td>");
  html += String(ESP.getFreeHeap());
  html += F(" bytes</td></tr></table>");
  return html;
}

}  // namespace

void setup() {
  // Records only non-deep-sleep resets to NVS; normal timer/GPIO wakes do not
  // write flash. This runs before the production state machine is touched.
  skmDiagnosticsBoot();

  if (skmServiceRequested()) {
    Serial.begin(115200);
    delay(250);
    skmRunServiceMode(FIRMWARE_VERSION,
                      FIRMWARE_FLAVOR,
                      buildServiceHardwareHtml);
  }

  // Jumper open: execute exactly the existing validated firmware path.
  skmNormalSetup();
}

void loop() {
  skmNormalLoop();
}
