#include <Arduino.h>
#include "esp_sleep.h"

#ifndef ZIGBEE_MODE_ED
#error "SkimmerSense must be built in Zigbee End Device mode"
#endif

#ifndef SKIMMERSENSE_SLEEP_SECONDS
#define SKIMMERSENSE_SLEEP_SECONDS 60ULL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_WAIT_MS
#define SKIMMERSENSE_ZIGBEE_WAIT_MS 10000UL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_IDLE_MS
#define SKIMMERSENSE_ZIGBEE_IDLE_MS 8000UL
#endif

#include "Zigbee.h"

static constexpr char FIRMWARE_VERSION[] = "0.9-deepsleep-zigbee-stage0";
static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
ZigbeeBinary zbLowLevel(ZB_EP_LOW_LEVEL);
ZigbeeBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

void configureZigbeeEndpoints() {
  // Keep the same endpoint/cluster descriptor already known by Zigbee2MQTT,
  // but do not change any runtime attribute after Zigbee connects.
  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(20.0);
  zbTemperature.setTolerance(1);
  zbTemperature.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100, 40);

  zbLowLevel.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbLowLevel.addBinaryInput();
  zbLowLevel.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  zbLowLevel.setBinaryInputDescription("Low Level");

  zbHighLevel.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbHighLevel.addBinaryInput();
  zbHighLevel.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  zbHighLevel.setBinaryInputDescription("High Level");

  Zigbee.addEndpoint(&zbTemperature);
  Zigbee.addEndpoint(&zbLowLevel);
  Zigbee.addEndpoint(&zbHighLevel);
}

bool startZigbee() {
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 10000;
  Zigbee.setTimeout(10000);

  Serial.println("Starting Zigbee sleepy End Device...");
  if (!Zigbee.begin(&zigbeeConfig, false)) {
    Serial.println("Zigbee begin failed");
    return false;
  }

  Serial.print("Waiting for Zigbee network");
  const uint32_t startedAt = millis();
  while (!Zigbee.connected() && millis() - startedAt < SKIMMERSENSE_ZIGBEE_WAIT_MS) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();

  if (!Zigbee.connected()) {
    Serial.println("Zigbee reconnect timeout");
    return false;
  }

  Serial.println("Zigbee connected!");
  return true;
}

void enterTimerSleep() {
  esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(SKIMMERSENSE_SLEEP_SECONDS) * 1000000ULL);
  Serial.printf("Deep sleep: %llu s timer only\n",
                static_cast<unsigned long long>(SKIMMERSENSE_SLEEP_SECONDS));
  Serial.println("Going to deep sleep now.");
  Serial.flush();
  delay(20);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("========================================");
  Serial.printf(" SkimmerSense v%s\n", FIRMWARE_VERSION);
  Serial.println(" Zigbee connect-only isolation test");
  Serial.println(" NO post-connect attribute updates");
  Serial.println(" NO explicit reports");
  Serial.println("========================================");

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause: %s\n",
                cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" :
                cause == ESP_SLEEP_WAKEUP_UNDEFINED ? "cold boot/reset" : "other");

  configureZigbeeEndpoints();
  const bool connected = startZigbee();

  if (connected) {
    Serial.printf("Stage0: Zigbee connected; idling %lu ms with no attribute changes...\n",
                  static_cast<unsigned long>(SKIMMERSENSE_ZIGBEE_IDLE_MS));
    delay(SKIMMERSENSE_ZIGBEE_IDLE_MS);
    Serial.println("Stage0: idle survived without crash.");
  }

  enterTimerSleep();
}

void loop() {
  delay(1000);
}
