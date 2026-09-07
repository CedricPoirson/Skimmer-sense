#include "service_mode.h"

// Pre-include every header used by sleep_main.cpp before the setup/loop rename
// macros are defined. This keeps those macros from ever touching framework or
// library declarations.
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "soc/soc_caps.h"
#include "Zigbee.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

// Keep the validated deep-sleep state-machine logic unchanged; the source now
// also emits observational RTC log hooks for SERVICE diagnostics. It is compiled
// inside this translation unit under private entry-point names;
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
  html.reserve(3400);

  if (snapshot.batteryValid) {
    const BatteryAssessment assessment = assessBattery(snapshot);
    const char *tone = "neutral";
    if (assessment == BatteryAssessment::FULL ||
        assessment == BatteryAssessment::CHARGING) {
      tone = "good";
    } else if (assessment == BatteryAssessment::LOW_LEVEL ||
               assessment == BatteryAssessment::STABILIZING) {
      tone = "warning";
    } else if (assessment == BatteryAssessment::CRITICAL) {
      tone = "critical";
    }

    float estimateHours = 0.0f;
    bool untilFull = false;
    const bool estimateValid =
        batteryEstimateHours(snapshot, estimateHours, untilFull);
    const String alerts = batteryAlertSummary(snapshot);

    html += F("<div class='batterybox ");
    html += tone;
    html += F("'><div class='batteryhead'><div><span class='eyebrow'>Batterie</span><div class='batterystate'>");
    html += batteryAssessmentName(assessment);
    html += F("</div></div><strong>");
    html += String(snapshot.batteryPercent);
    html += F(" %</strong></div><div class='batterytrack'><span style='width:");
    html += String(snapshot.batteryPercent);
    html += F("%'></span></div><div class='batteryfacts'>");

    html += F("<div><span>Tension</span><b>");
    html += String(snapshot.batteryVoltage, 3);
    html += F(" V</b></div><div><span>Évolution</span><b>");
    if (snapshot.batteryRateValid) {
      if (snapshot.batteryRatePercentPerHour >= 0.0f) html += '+';
      html += String(snapshot.batteryRatePercentPerHour, 2);
      html += F(" %/h");
    } else {
      html += F("indisponible");
    }
    html += F("</b></div><div><span>Estimation</span><b>");
    if (estimateValid) {
      const uint16_t totalMinutes =
          static_cast<uint16_t>(estimateHours * 60.0f + 0.5f);
      html += untilFull ? F("pleine dans ") : F("autonomie ");
      if (totalMinutes >= 60) {
        html += String(totalMinutes / 60);
        html += F(" h ");
      }
      html += String(totalMinutes % 60);
      html += F(" min");
    } else {
      html += F("en observation");
    }
    html += F("</b></div><div><span>Alertes</span><b>");
    html += alerts;
    html += F("</b></div></div>");

    if (assessment == BatteryAssessment::STABILIZING) {
      html += F("<div class='info'>Le MAX17048 stabilise son estimation après insertion ou variation rapide.</div>");
    }
    html += F("</div>");
  } else {
    html += F("<div class='batterybox critical'><div class='batterystate'>Batterie indisponible</div></div>");
  }

  html += F("<h3 style='margin-top:20px'>Capteurs et état interne</h3><table>");
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

  html += F("<tr><th>Adaptive NORMAL cache</th><td>");
  if (rtcNormalSleepValid) {
    html += String(static_cast<unsigned long>(rtcNormalSleepSeconds));
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
