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
  zbTemperature.setDefaultValue(25.0);
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


void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SkimmerSense Zigbee endpoint test");
  Serial.println(" ENDPOINTS ONLY");
  Serial.println(" NO REPORTS");
  Serial.println("========================================");


  if (!configureEndpoints()) {
    Serial.println("Endpoint configuration FAILED");
    return;
  }

  Serial.println("Endpoints configured");


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

  Serial.println("Zigbee endpoint test OK");

  esp_sleep_enable_timer_wakeup(
      1800ULL * 1000000ULL);

  Serial.println("Going to deep sleep");

  delay(100);

  esp_deep_sleep_start();
}


void loop()
{
}
