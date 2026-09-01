#include <Arduino.h>
#include "Zigbee.h"
#include "esp_sleep.h"

static constexpr uint8_t EP_TEMP = 10;
static constexpr uint8_t EP_LOW  = 11;
static constexpr uint8_t EP_HIGH = 12;

ZigbeeTempSensor zbTemperature(EP_TEMP);
ZigbeeBinary zbLowLevel(EP_LOW);
ZigbeeBinary zbHighLevel(EP_HIGH);


bool configureEndpoints()
{
  bool ok = true;

  zbTemperature.setManufacturerAndModel(
      "SkimmerSense",
      "SkimmerSense-v1");

  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(25.5);
  zbTemperature.setTolerance(1);

  ok &= zbLowLevel.addBinaryInput();
  ok &= zbLowLevel.setBinaryInputDescription("Low Level");

  ok &= zbHighLevel.addBinaryInput();
  ok &= zbHighLevel.setBinaryInputDescription("High Level");

  ok &= Zigbee.addEndpoint(&zbTemperature);
  ok &= Zigbee.addEndpoint(&zbLowLevel);
  ok &= Zigbee.addEndpoint(&zbHighLevel);

  return ok;
}


bool sendTemperatureReport()
{
  esp_zb_zcl_report_attr_cmd_t report{};

  report.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  report.attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID;
  report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
  report.zcl_basic_cmd.src_endpoint = EP_TEMP;
  report.manuf_specific = 0;
  report.dis_default_resp = 0;

  Serial.println("Report temperature : queue...");

  if (!esp_zb_lock_acquire(portMAX_DELAY)) {
    Serial.println("Zigbee lock FAILED");
    return false;
  }

  esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report);

  esp_zb_lock_release();

  if (err != ESP_OK) {
    Serial.printf("Report FAILED: 0x%x\n", err);
    return false;
  }

  Serial.println("Report temperature : queued OK");
  return true;
}


void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SkimmerSense Zigbee temperature report test");
  Serial.println(" ENDPOINTS + PRELOAD + ONE REPORT");
  Serial.println("========================================");


  if (!configureEndpoints()) {
    Serial.println("Endpoint configuration FAILED");
    return;
  }

  Serial.println("Endpoints configured");
  Serial.println("Attributes preloaded");


  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 10000;

  Zigbee.setTimeout(10000);

  Serial.println("Starting Zigbee sleepy End Device...");

  if (!Zigbee.begin(&zigbeeConfig, false)) {
    Serial.println("Zigbee begin failed");
    return;
  }


  Serial.println("Waiting Zigbee network");

  while (!Zigbee.connected()) {
    delay(100);
  }

  Serial.println("Zigbee connected!");

  delay(8000);


  sendTemperatureReport();


  Serial.println("Post report wait...");
  delay(2000);

  Serial.println("Temperature report test OK");

  esp_sleep_enable_timer_wakeup(
      1800ULL * 1000000ULL);

  Serial.println("Going to deep sleep");

  delay(100);

  esp_deep_sleep_start();
}


void loop()
{
}
