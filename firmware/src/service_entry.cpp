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

String max17048Hex(uint16_t value) {
  char text[7];
  snprintf(text, sizeof(text), "0x%04X", static_cast<unsigned>(value));
  return String(text);
}

void appendMax17048RegisterRow(String &html,
                               const __FlashStringHelper *name,
                               uint8_t address,
                               bool valid,
                               uint16_t raw,
                               const String &meaning) {
  html += F("<tr><th>");
  html += name;
  html += F(" <code>0x");
  if (address < 0x10) html += '0';
  html += String(address, HEX);
  html += F("</code></th><td>");
  if (!valid) {
    html += F("<span style='color:var(--red)'>lecture impossible</span>");
  } else {
    html += F("<code>");
    html += max17048Hex(raw);
    html += F("</code><br><span class='sub'>");
    html += meaning;
    html += F("</span>");
  }
  html += F("</td></tr>");
}

String buildMax17048AdvancedHtml(const SensorSnapshot &snapshot) {
  uint16_t vcell = 0, soc = 0, mode = 0, version = 0, hibrt = 0;
  uint16_t config = 0, valrt = 0, crate = 0, vresetId = 0, status = 0;
  const bool vcellOk = readRegister16(MAX17048_REG_VCELL, vcell);
  const bool socOk = readRegister16(MAX17048_REG_SOC, soc);
  const bool modeOk = readRegister16(MAX17048_REG_MODE, mode);
  const bool versionOk = readRegister16(MAX17048_REG_VERSION, version);
  const bool hibrtOk = readRegister16(MAX17048_REG_HIBRT, hibrt);
  const bool configOk = readRegister16(MAX17048_REG_CONFIG, config);
  const bool valrtOk = readRegister16(MAX17048_REG_VALRT, valrt);
  const bool crateOk = readRegister16(MAX17048_REG_CRATE, crate);
  const bool vresetOk = readRegister16(MAX17048_REG_VRESET_ID, vresetId);
  const bool statusOk = readRegister16(MAX17048_REG_STATUS, status);

  String html;
  html.reserve(4300);
  html += F("<details style='margin-top:18px'><summary style='cursor:pointer;font-weight:800;color:var(--cyan)'>Diagnostic avancé MAX17048</summary>");
  html += F("<div class='info'>Lecture seule : aucun QuickStart, reset ou changement de configuration n’est effectué.</div><table>");

  appendMax17048RegisterRow(
      html, F("VCELL"), MAX17048_REG_VCELL, vcellOk, vcell,
      vcellOk ? String(static_cast<float>(vcell) * 78.125f / 1000000.0f, 4) + F(" V — tension mesurée")
              : String());
  appendMax17048RegisterRow(
      html, F("SOC"), MAX17048_REG_SOC, socOk, soc,
      socOk ? String(static_cast<float>(soc) / 256.0f, 2) + F(" % — estimation ModelGauge non arrondie")
            : String());

  String modeText;
  if (modeOk) {
    modeText = String(F("QuickStart=")) + ((mode & 0x4000U) ? F("1") : F("0"));
    modeText += F(" · EnSleep=");
    modeText += (mode & 0x2000U) ? F("1") : F("0");
    modeText += F(" · HibStat=");
    modeText += (mode & 0x1000U) ? F("hibernation") : F("actif");
  }
  appendMax17048RegisterRow(
      html, F("MODE"), MAX17048_REG_MODE, modeOk, mode, modeText);

  appendMax17048RegisterRow(
      html, F("VERSION"), MAX17048_REG_VERSION, versionOk, version,
      versionOk ? String(F("version de production du circuit")) : String());

  String hibrtText;
  if (hibrtOk) {
    const float hibRate =
        static_cast<float>(static_cast<uint8_t>(hibrt >> 8)) * 0.208f;
    const float activeMv =
        static_cast<float>(static_cast<uint8_t>(hibrt & 0xFF)) * 1.25f;
    hibrtText = F("entrée hibernation sous ");
    hibrtText += String(hibRate, 2);
    hibrtText += F(" %/h pendant 6 min · sortie si variation OCV > ");
    hibrtText += String(activeMv, 2);
    hibrtText += F(" mV");
  }
  appendMax17048RegisterRow(
      html, F("HIBRT"), MAX17048_REG_HIBRT, hibrtOk, hibrt, hibrtText);

  String configText;
  if (configOk) {
    const uint8_t flags = static_cast<uint8_t>(config & 0xFF);
    configText = F("RCOMP=");
    configText += String(static_cast<uint8_t>(config >> 8));
    configText += F(" · SLEEP=");
    configText += (flags & 0x80U) ? F("1") : F("0");
    configText += F(" · alerte variation SOC=");
    configText += (flags & 0x40U) ? F("active") : F("inactive");
    configText += F(" · ALRT=");
    configText += (flags & 0x20U) ? F("active") : F("inactive");
    configText += F(" · seuil SOC=");
    configText += String(32U - (flags & 0x1FU));
    configText += F(" %");
  }
  appendMax17048RegisterRow(
      html, F("CONFIG"), MAX17048_REG_CONFIG, configOk, config, configText);

  String valrtText;
  if (valrtOk) {
    valrtText = F("minimum ");
    valrtText += String(static_cast<uint8_t>(valrt >> 8) * 0.020f, 2);
    valrtText += F(" V · maximum ");
    valrtText += String(static_cast<uint8_t>(valrt & 0xFF) * 0.020f, 2);
    valrtText += F(" V");
  }
  appendMax17048RegisterRow(
      html, F("VALRT"), MAX17048_REG_VALRT, valrtOk, valrt, valrtText);

  appendMax17048RegisterRow(
      html, F("CRATE"), MAX17048_REG_CRATE, crateOk, crate,
      crateOk
        ? String(static_cast<float>(static_cast<int16_t>(crate)) *
                 MAX17048_CRATE_LSB_PERCENT_PER_HOUR, 2) +
            F(" %/h — tendance du gauge, pas un courant")
        : String());

  String vresetText;
  if (vresetOk) {
    const uint8_t resetByte = static_cast<uint8_t>(vresetId >> 8);
    vresetText = F("seuil reset ");
    vresetText += String(static_cast<float>(resetByte >> 1) * 0.040f, 2);
    vresetText += F(" V · comparateur hibernation ");
    vresetText += (resetByte & 0x01U) ? F("désactivé") : F("activé");
    vresetText += F(" · ID=");
    vresetText += String(static_cast<uint8_t>(vresetId & 0xFF));
  }
  appendMax17048RegisterRow(
      html, F("VRESET/ID"), MAX17048_REG_VRESET_ID,
      vresetOk, vresetId, vresetText);

  String statusText;
  if (statusOk) {
    const uint8_t liveStatus = static_cast<uint8_t>(status >> 8);
    statusText = F("EnVR=");
    statusText += (liveStatus & 0x40U) ? F("1") : F("0");
    statusText += F(" · drapeaux capturés avant acquittement : ");
    statusText += batteryAlertSummary(snapshot);
  }
  appendMax17048RegisterRow(
      html, F("STATUS"), MAX17048_REG_STATUS, statusOk, status, statusText);

  html += F("</table></details>");
  return html;
}

String buildServiceHardwareHtml() {
  SensorSnapshot snapshot = readBaseSensorSnapshot();
  readTemperatureIntoSnapshot(snapshot);

  String html;
  html.reserve(7800);

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

  html += buildMax17048AdvancedHtml(snapshot);

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
