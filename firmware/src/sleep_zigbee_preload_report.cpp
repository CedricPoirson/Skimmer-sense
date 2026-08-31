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

#ifndef SKIMMERSENSE_PRELOAD_REPORT_DELAY_MS
#define SKIMMERSENSE_PRELOAD_REPORT_DELAY_MS 8000UL
#endif

#ifndef SKIMMERSENSE_POST_REPORT_WAIT_MS
#define SKIMMERSENSE_POST_REPORT_WAIT_MS 2000UL
#endif

#include "Zigbee.h"

static constexpr char FIRMWARE_VERSION[] = "0.9-deepsleep-zigbee-preload-report";
static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;
static constexpr float TEST_TEMPERATURE_C = 25.0f;

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
ZigbeeBinary zbLowLevel(ZB_EP_LOW_LEVEL);
ZigbeeBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

void configureZigbeeEndpointsWithPreloadedTemperature() {
  // IMPORTANT: setDefaultValue() edits the endpoint's cluster list BEFORE
  // Zigbee.begin(). It does not call runtime esp_zb_zcl_set_attribute_val(),
  // which is the path that triggers the ZBOSS automatic-reporting crash seen
  // in stage1/stage1_wait8.
  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(TEST_TEMPERATURE_C);
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

bool sendZeroInitializedTemperatureReport() {
  // Arduino-ESP32 3.3.11 reportTemperature() leaves part of this command
  // structure uninitialized. Build the command ourselves and zero it first.
  esp_zb_zcl_report_attr_cmd_t report{};
  report.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  report.attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID;
  report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
  report.zcl_basic_cmd.src_endpoint = ZB_EP_TEMPERATURE;
  report.manuf_specific = 0x00U;
  report.dis_default_resp = 0x00U;

  if (!esp_zb_lock_acquire(portMAX_DELAY)) {
    Serial.println("Safe explicit report: Zigbee lock acquire FAILED");
    return false;
  }

  const esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report);
  esp_zb_lock_release();

  if (err != ESP_OK) {
    Serial.printf("Safe explicit report: queue FAILED: 0x%x (%s)\n",
                  err, esp_err_to_name(err));
    return false;
  }

  Serial.println("Safe explicit report: queued OK");
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

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("========================================");
  Serial.printf(" SkimmerSense v%s\n", FIRMWARE_VERSION);
  Serial.println(" PRELOAD temperature before Zigbee.begin()");
  Serial.println(" NO runtime setTemperature()");
  Serial.println(" ONE zero-initialized explicit report");
  Serial.println("========================================");

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause: %s\n",
                cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" :
                cause == ESP_SLEEP_WAKEUP_UNDEFINED ? "cold boot/reset" : "other");

  Serial.printf("Preloading endpoint 10 temperature to %.2f C BEFORE Zigbee.begin()...\n",
                TEST_TEMPERATURE_C);
  configureZigbeeEndpointsWithPreloadedTemperature();
  Serial.println("Preload complete.");

  if (!startZigbee()) {
    enterTimerSleep();
  }

  Serial.printf("Connected; idling %lu ms without runtime attribute writes...\n",
                static_cast<unsigned long>(SKIMMERSENSE_PRELOAD_REPORT_DELAY_MS));
  delay(SKIMMERSENSE_PRELOAD_REPORT_DELAY_MS);

  Serial.println("About to send zero-initialized explicit temperature report...");
  sendZeroInitializedTemperatureReport();

  Serial.printf("Post-report wait: %lu ms...\n",
                static_cast<unsigned long>(SKIMMERSENSE_POST_REPORT_WAIT_MS));
  delay(SKIMMERSENSE_POST_REPORT_WAIT_MS);
  Serial.println("Preload/report stage survived.");

  enterTimerSleep();
}

void loop() {
  delay(1000);
}
