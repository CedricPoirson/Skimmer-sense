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

#ifndef SKIMMERSENSE_TASK_START_DELAY_MS
#define SKIMMERSENSE_TASK_START_DELAY_MS 1000UL
#endif

#ifndef SKIMMERSENSE_REPORT_WAIT_MS
#define SKIMMERSENSE_REPORT_WAIT_MS 2000UL
#endif

#include "Zigbee.h"

static constexpr char FIRMWARE_VERSION[] = "0.9-deepsleep-zigbee-stage1-task";
static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
ZigbeeBinary zbLowLevel(ZB_EP_LOW_LEVEL);
ZigbeeBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

volatile bool reportResponseSeen = false;
volatile esp_zb_zcl_status_t reportResponseStatus = ESP_ZB_ZCL_STATUS_SUCCESS;

void onGlobalResponse(zb_cmd_type_t command,
                      esp_zb_zcl_status_t status,
                      uint8_t endpoint,
                      uint16_t cluster) {
  Serial.printf("Zigbee response: command=%d status=%s ep=%u cluster=0x%04X\n",
                static_cast<int>(command),
                esp_zb_zcl_status_to_name(status),
                endpoint,
                cluster);

  if (command == ZB_CMD_REPORT_ATTRIBUTE && endpoint == ZB_EP_TEMPERATURE) {
    reportResponseStatus = status;
    reportResponseSeen = true;
  }
}

void configureZigbeeEndpoints() {
  // Keep exactly the same endpoint descriptor already interviewed by Zigbee2MQTT.
  // Runtime test below touches ONLY the temperature attribute on endpoint 10.
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

  Zigbee.onGlobalDefaultResponse(onGlobalResponse);
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

[[noreturn]] void enterTimerSleep() {
  esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(SKIMMERSENSE_SLEEP_SECONDS) * 1000000ULL);
  Serial.printf("Deep sleep: %llu s timer only\n",
                static_cast<unsigned long long>(SKIMMERSENSE_SLEEP_SECONDS));
  Serial.println("Going to deep sleep now.");
  Serial.flush();
  delay(20);
  esp_deep_sleep_start();
  while (true) delay(1000);
}

static void temperatureReportTask(void *arg) {
  (void)arg;

  Serial.printf("Stage1-task: waiting %lu ms before touching Zigbee attributes...\n",
                static_cast<unsigned long>(SKIMMERSENSE_TASK_START_DELAY_MS));
  delay(SKIMMERSENSE_TASK_START_DELAY_MS);

  // Deliberately use a constant first. This isolates the Zigbee API from the
  // DS18B20, battery gauge and float code. Hardware sensing returns later once
  // the Zigbee path itself is proven stable.
  constexpr float TEST_TEMPERATURE_C = 25.0f;

  Serial.println("Stage1-task: setTemperature(25.00) -> start");
  const bool setOk = zbTemperature.setTemperature(TEST_TEMPERATURE_C);
  Serial.printf("Stage1-task: setTemperature() -> %s\n", setOk ? "OK" : "FAILED");

  if (!setOk) {
    enterTimerSleep();
  }

  delay(250);

  reportResponseSeen = false;
  Serial.println("Stage1-task: reportTemperature() -> start");
  const bool reportQueued = zbTemperature.reportTemperature();
  Serial.printf("Stage1-task: reportTemperature() queued -> %s\n",
                reportQueued ? "YES" : "NO");

  const uint32_t waitStartedAt = millis();
  while (!reportResponseSeen && millis() - waitStartedAt < SKIMMERSENSE_REPORT_WAIT_MS) {
    delay(25);
  }

  if (reportResponseSeen) {
    Serial.printf("Stage1-task: report response -> %s\n",
                  esp_zb_zcl_status_to_name(reportResponseStatus));
  } else {
    Serial.println("Stage1-task: no default response before timeout (not fatal for this test).");
  }

  Serial.println("Stage1-task: temperature-only Zigbee path survived.");
  enterTimerSleep();
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("========================================");
  Serial.printf(" SkimmerSense v%s\n", FIRMWARE_VERSION);
  Serial.println(" Zigbee temperature-only task test");
  Serial.println(" ONLY endpoint 10 temperature is changed/reported");
  Serial.println(" NO binary/battery runtime updates");
  Serial.println("========================================");

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause: %s\n",
                cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" :
                cause == ESP_SLEEP_WAKEUP_UNDEFINED ? "cold boot/reset" : "other");

  configureZigbeeEndpoints();
  if (!startZigbee()) {
    enterTimerSleep();
  }

  const BaseType_t taskCreated = xTaskCreate(
      temperatureReportTask,
      "skimmer_zb_temp",
      4096,
      nullptr,
      10,
      nullptr);

  if (taskCreated != pdPASS) {
    Serial.println("Stage1-task: xTaskCreate failed");
    enterTimerSleep();
  }

  Serial.println("Stage1-task: worker created; setup() returning.");
}

void loop() {
  delay(100);
}
