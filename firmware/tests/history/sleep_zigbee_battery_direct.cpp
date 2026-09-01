#include <Arduino.h>
#include <Wire.h>
#include "esp_sleep.h"

#ifndef ZIGBEE_MODE_ED
#error "SkimmerSense must be built in Zigbee End Device mode"
#endif

#include "Zigbee.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#ifndef SKIMMERSENSE_SLEEP_SECONDS
#define SKIMMERSENSE_SLEEP_SECONDS 60ULL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_WAIT_MS
#define SKIMMERSENSE_ZIGBEE_WAIT_MS 10000UL
#endif

#ifndef SKIMMERSENSE_DIRECT_REPORT_DELAY_MS
#define SKIMMERSENSE_DIRECT_REPORT_DELAY_MS 15000UL
#endif

#ifndef SKIMMERSENSE_POST_REPORT_WAIT_MS
#define SKIMMERSENSE_POST_REPORT_WAIT_MS 2000UL
#endif

static constexpr char FIRMWARE_VERSION[] = "0.9-zigbee-battery-direct-test";

static constexpr uint8_t PIN_I2C_SDA = D4;
static constexpr uint8_t PIN_I2C_SCL = D5;
static constexpr uint8_t MAX17048_I2C_ADDRESS = 0x36;
static constexpr uint8_t MAX17048_REG_VCELL = 0x02;
static constexpr uint8_t MAX17048_REG_SOC = 0x04;
static constexpr uint8_t MAX17048_REG_VERSION = 0x08;

static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;
static constexpr uint16_t COORDINATOR_SHORT_ADDRESS = 0x0000;
static constexpr uint8_t COORDINATOR_ENDPOINT = 1;

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
ZigbeeBinary zbLowLevel(ZB_EP_LOW_LEVEL);
ZigbeeBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

struct BatterySnapshot {
  bool valid = false;
  uint16_t version = 0;
  float voltage = 4.0f;
  float socRaw = 100.0f;
  uint8_t percent = 100;
  uint8_t voltageZcl = 40;
};

bool readRegister16(uint8_t reg, uint16_t &value) {
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MAX17048_I2C_ADDRESS, static_cast<uint8_t>(2)) != 2) return false;
  value = (static_cast<uint16_t>(Wire.read()) << 8) |
          static_cast<uint16_t>(Wire.read());
  return true;
}

BatterySnapshot readBattery() {
  BatterySnapshot battery;
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  uint16_t rawVcell = 0;
  uint16_t rawSoc = 0;
  uint16_t version = 0;
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  if (Wire.endTransmission() != 0 ||
      !readRegister16(MAX17048_REG_VERSION, version) ||
      !readRegister16(MAX17048_REG_VCELL, rawVcell) ||
      !readRegister16(MAX17048_REG_SOC, rawSoc)) {
    return battery;
  }

  battery.valid = true;
  battery.version = version;
  battery.voltage = static_cast<float>(rawVcell) * 78.125f / 1000000.0f;
  battery.socRaw = static_cast<float>(rawSoc) / 256.0f;

  float soc = battery.socRaw;
  if (soc < 0.0f) soc = 0.0f;
  if (soc > 100.0f) soc = 100.0f;
  battery.percent = static_cast<uint8_t>(soc + 0.5f);

  int voltage100mV = static_cast<int>(battery.voltage * 10.0f + 0.5f);
  if (voltage100mV < 0) voltage100mV = 0;
  if (voltage100mV > 255) voltage100mV = 255;
  battery.voltageZcl = static_cast<uint8_t>(voltage100mV);
  return battery;
}

void configureEndpoints(const BatterySnapshot &battery) {
  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(25.0f);
  zbTemperature.setTolerance(1);
  zbTemperature.setPowerSource(ZB_POWER_SOURCE_BATTERY,
                               battery.percent,
                               battery.voltageZcl);

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

bool sendBatteryDirectToCoordinator() {
  esp_zb_zcl_report_attr_cmd_t report{};
  report.zcl_basic_cmd.dst_addr_u.addr_short = COORDINATOR_SHORT_ADDRESS;
  report.zcl_basic_cmd.dst_endpoint = COORDINATOR_ENDPOINT;
  report.zcl_basic_cmd.src_endpoint = ZB_EP_TEMPERATURE;
  report.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  report.clusterID = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
  report.attributeID = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID;
  report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report.manuf_specific = 0x00U;
  report.dis_default_resp = 0x00U;

  Serial.println("Battery direct report -> coordinator 0x0000 endpoint 1: queue...");
  if (!esp_zb_lock_acquire(portMAX_DELAY)) {
    Serial.println("Battery direct report: Zigbee lock FAILED");
    return false;
  }
  const esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report);
  esp_zb_lock_release();

  if (err != ESP_OK) {
    Serial.printf("Battery direct report: FAILED 0x%x (%s)\n", err, esp_err_to_name(err));
    return false;
  }
  Serial.println("Battery direct report: queued OK");
  return true;
}

[[noreturn]] void sleepNow() {
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
  Serial.println(" Battery-only direct-address report test");
  Serial.println(" Power Config is preloaded BEFORE Zigbee.begin()");
  Serial.println(" Report bypasses binding table");
  Serial.println("========================================");

  const BatterySnapshot battery = readBattery();
  if (battery.valid) {
    Serial.printf("MAX17048: VERSION=0x%04X | %.3f V | raw SOC %.1f %% | Zigbee %u %%\n",
                  battery.version, battery.voltage, battery.socRaw, battery.percent);
  } else {
    Serial.println("MAX17048 unavailable; using fallback 100 % / 4.0 V");
  }

  Serial.println("Preloading battery cluster before Zigbee.begin()...");
  configureEndpoints(battery);
  Serial.println("Preload complete.");

  if (!startZigbee()) {
    sleepNow();
  }

  Serial.printf("Connected; waiting %lu ms before direct battery report...\n",
                static_cast<unsigned long>(SKIMMERSENSE_DIRECT_REPORT_DELAY_MS));
  delay(SKIMMERSENSE_DIRECT_REPORT_DELAY_MS);

  sendBatteryDirectToCoordinator();

  Serial.printf("Post-report wait: %lu ms...\n",
                static_cast<unsigned long>(SKIMMERSENSE_POST_REPORT_WAIT_MS));
  delay(SKIMMERSENSE_POST_REPORT_WAIT_MS);
  Serial.println("Battery direct-report stage survived.");
  sleepNow();
}

void loop() {
  delay(1000);
}
