#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

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

#ifndef SKIMMERSENSE_BETWEEN_REPORTS_MS
#define SKIMMERSENSE_BETWEEN_REPORTS_MS 400UL
#endif

#ifndef SKIMMERSENSE_POST_REPORT_WAIT_MS
#define SKIMMERSENSE_POST_REPORT_WAIT_MS 2000UL
#endif

#include "Zigbee.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

static constexpr char FIRMWARE_VERSION[] = "0.9-deepsleep-zigbee-snapshot-reports";

// Seeed Studio XIAO ESP32-C6 pinout.
static constexpr uint8_t PIN_FLOAT_LOW = D0;       // GPIO0, reed to GND
static constexpr uint8_t PIN_FLOAT_HIGH = D1;      // GPIO1, reed to GND
static constexpr uint8_t PIN_DS18B20_POWER = D2;
static constexpr uint8_t PIN_DS18B20_DATA = D3;
static constexpr uint8_t PIN_I2C_SDA = D4;
static constexpr uint8_t PIN_I2C_SCL = D5;
static constexpr uint8_t PIN_MAX17048_INT = 4;     // GPIO4 / MTMS -> ALRT/INT

static constexpr uint8_t MAX17048_I2C_ADDRESS = 0x36;
static constexpr uint8_t MAX17048_REG_VCELL = 0x02;
static constexpr uint8_t MAX17048_REG_SOC = 0x04;
static constexpr uint8_t MAX17048_REG_VERSION = 0x08;

static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

// ZigbeeBinary has no public pre-registration setter for PresentValue.  Use a
// tiny derived class so the value can be written directly into the cluster list
// BEFORE Zigbee.begin().  This avoids runtime esp_zb_zcl_set_attribute_val(),
// whose automatic-reporting path is the crash we isolated in ZBOSS.
class PreloadBinary : public ZigbeeBinary {
 public:
  explicit PreloadBinary(uint8_t endpoint) : ZigbeeBinary(endpoint) {}

  bool preloadBinaryInput(bool value) {
    esp_zb_attribute_list_t *cluster = esp_zb_cluster_list_get_cluster(
        _cluster_list,
        ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (cluster == nullptr) {
      return false;
    }
    return esp_zb_cluster_update_attr(
               cluster,
               ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
               &value) == ESP_OK;
  }
};

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
PreloadBinary zbLowLevel(ZB_EP_LOW_LEVEL);
PreloadBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

struct SensorSnapshot {
  bool lowClosed = false;
  bool highClosed = false;
  bool batteryValid = false;
  uint16_t maxVersion = 0;
  float batteryVoltage = 0.0f;
  float batterySocRaw = 0.0f;
  uint8_t batteryPercent = 100;
  uint8_t batteryVoltageZcl = 40;  // 100 mV units
  bool temperatureValid = false;
  float waterTemperatureC = 20.0f;
};

const char *contactState(bool closed) {
  return closed ? "CLOSED" : "OPEN";
}

const char *wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1 GPIO";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "cold boot/reset";
    default: return "other";
  }
}

void releaseRtcWakePins() {
  rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_FLOAT_LOW));
  rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_FLOAT_HIGH));
  rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_MAX17048_INT));
}

bool readRegister16(uint8_t reg, uint16_t &value) {
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MAX17048_I2C_ADDRESS, static_cast<uint8_t>(2)) != 2) return false;
  value = (static_cast<uint16_t>(Wire.read()) << 8) |
          static_cast<uint16_t>(Wire.read());
  return true;
}

float readWaterTemperatureC() {
  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, HIGH);
  delay(20);

  temperatureSensors.begin();
  if (temperatureSensors.getDeviceCount() == 0) {
    pinMode(PIN_DS18B20_DATA, INPUT);
    digitalWrite(PIN_DS18B20_POWER, LOW);
    return DEVICE_DISCONNECTED_C;
  }

  temperatureSensors.setResolution(10);
  temperatureSensors.requestTemperatures();
  const float value = temperatureSensors.getTempCByIndex(0);

  pinMode(PIN_DS18B20_DATA, INPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  return value;
}

SensorSnapshot readSensorSnapshot() {
  SensorSnapshot snapshot;

  releaseRtcWakePins();
  pinMode(PIN_FLOAT_LOW, INPUT_PULLUP);
  pinMode(PIN_FLOAT_HIGH, INPUT_PULLUP);
  pinMode(PIN_MAX17048_INT, INPUT_PULLUP);
  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  pinMode(PIN_DS18B20_DATA, INPUT);

  snapshot.lowClosed = digitalRead(PIN_FLOAT_LOW) == LOW;
  snapshot.highClosed = digitalRead(PIN_FLOAT_HIGH) == LOW;

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  uint16_t rawVcell = 0;
  uint16_t rawSoc = 0;
  uint16_t version = 0;
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  if (Wire.endTransmission() == 0 &&
      readRegister16(MAX17048_REG_VERSION, version) &&
      readRegister16(MAX17048_REG_VCELL, rawVcell) &&
      readRegister16(MAX17048_REG_SOC, rawSoc)) {
    snapshot.batteryValid = true;
    snapshot.maxVersion = version;
    snapshot.batteryVoltage = static_cast<float>(rawVcell) * 78.125f / 1000000.0f;
    snapshot.batterySocRaw = static_cast<float>(rawSoc) / 256.0f;

    float clampedSoc = snapshot.batterySocRaw;
    if (clampedSoc < 0.0f) clampedSoc = 0.0f;
    if (clampedSoc > 100.0f) clampedSoc = 100.0f;
    snapshot.batteryPercent = static_cast<uint8_t>(clampedSoc + 0.5f);

    int voltage100mV = static_cast<int>(snapshot.batteryVoltage * 10.0f + 0.5f);
    if (voltage100mV < 0) voltage100mV = 0;
    if (voltage100mV > 255) voltage100mV = 255;
    snapshot.batteryVoltageZcl = static_cast<uint8_t>(voltage100mV);
  }

  const float temperature = readWaterTemperatureC();
  if (temperature != DEVICE_DISCONNECTED_C && temperature > -55.0f && temperature < 85.0f) {
    snapshot.temperatureValid = true;
    snapshot.waterTemperatureC = temperature;
  }

  return snapshot;
}

bool configureZigbeeEndpoints(const SensorSnapshot &snapshot) {
  bool ok = true;

  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(snapshot.waterTemperatureC);
  zbTemperature.setTolerance(1);
  ok &= zbTemperature.setPowerSource(
      ZB_POWER_SOURCE_BATTERY,
      snapshot.batteryPercent,
      snapshot.batteryVoltageZcl);

  zbLowLevel.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  ok &= zbLowLevel.addBinaryInput();
  ok &= zbLowLevel.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  ok &= zbLowLevel.setBinaryInputDescription("Low Level");
  ok &= zbLowLevel.preloadBinaryInput(snapshot.lowClosed);

  zbHighLevel.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  ok &= zbHighLevel.addBinaryInput();
  ok &= zbHighLevel.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  ok &= zbHighLevel.setBinaryInputDescription("High Level");
  ok &= zbHighLevel.preloadBinaryInput(snapshot.highClosed);

  ok &= Zigbee.addEndpoint(&zbTemperature);
  ok &= Zigbee.addEndpoint(&zbLowLevel);
  ok &= Zigbee.addEndpoint(&zbHighLevel);
  return ok;
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

bool sendSafeReport(uint8_t endpoint, uint16_t clusterId, uint16_t attributeId, const char *label) {
  esp_zb_zcl_report_attr_cmd_t report{};
  report.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  report.attributeID = attributeId;
  report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report.clusterID = clusterId;
  report.zcl_basic_cmd.src_endpoint = endpoint;
  report.manuf_specific = 0x00U;
  report.dis_default_resp = 0x00U;

  Serial.printf("Report %-12s: queue...\n", label);
  if (!esp_zb_lock_acquire(portMAX_DELAY)) {
    Serial.printf("Report %-12s: Zigbee lock FAILED\n", label);
    return false;
  }

  const esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report);
  esp_zb_lock_release();

  if (err != ESP_OK) {
    Serial.printf("Report %-12s: FAILED 0x%x (%s)\n", label, err, esp_err_to_name(err));
    return false;
  }

  Serial.printf("Report %-12s: queued OK\n", label);
  return true;
}

[[noreturn]] void enterTimerSleep() {
  pinMode(PIN_DS18B20_DATA, INPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);

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
  Serial.println(" REAL sensor snapshot before Zigbee.begin()");
  Serial.println(" NO runtime attribute mutations");
  Serial.println(" ZERO-initialized explicit reports");
  Serial.println("========================================");
  Serial.printf("Wake cause: %s\n", wakeCauseName(esp_sleep_get_wakeup_cause()));

  const SensorSnapshot snapshot = readSensorSnapshot();
  Serial.printf("Float LOW : %s\n", contactState(snapshot.lowClosed));
  Serial.printf("Float HIGH: %s\n", contactState(snapshot.highClosed));
  if (snapshot.temperatureValid) {
    Serial.printf("Water temperature: %.2f C\n", snapshot.waterTemperatureC);
  } else {
    Serial.printf("Water temperature invalid; fallback preload: %.2f C\n", snapshot.waterTemperatureC);
  }
  if (snapshot.batteryValid) {
    Serial.printf("MAX17048: VERSION=0x%04X | %.3f V | raw SOC %.1f %% | Zigbee %u %%\n",
                  snapshot.maxVersion,
                  snapshot.batteryVoltage,
                  snapshot.batterySocRaw,
                  snapshot.batteryPercent);
  } else {
    Serial.printf("MAX17048 unavailable; fallback Zigbee battery: %u %% / %u.%u V\n",
                  snapshot.batteryPercent,
                  snapshot.batteryVoltageZcl / 10,
                  snapshot.batteryVoltageZcl % 10);
  }

  Serial.println("Preloading all Zigbee attributes BEFORE Zigbee.begin()...");
  if (!configureZigbeeEndpoints(snapshot)) {
    Serial.println("Preload/configuration FAILED; sleeping without starting Zigbee.");
    enterTimerSleep();
  }
  Serial.println("Preload complete.");

  if (!startZigbee()) {
    enterTimerSleep();
  }

  Serial.printf("Connected; idling %lu ms without runtime attribute writes...\n",
                static_cast<unsigned long>(SKIMMERSENSE_PRELOAD_REPORT_DELAY_MS));
  delay(SKIMMERSENSE_PRELOAD_REPORT_DELAY_MS);

  bool reportsOk = true;
  if (snapshot.temperatureValid) {
    reportsOk &= sendSafeReport(
        ZB_EP_TEMPERATURE,
        ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
        "temperature");
    delay(SKIMMERSENSE_BETWEEN_REPORTS_MS);
  } else {
    Serial.println("Report temperature : skipped (invalid DS18B20 reading)");
  }

  reportsOk &= sendSafeReport(
      ZB_EP_LOW_LEVEL,
      ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
      ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
      "low-float");
  delay(SKIMMERSENSE_BETWEEN_REPORTS_MS);

  reportsOk &= sendSafeReport(
      ZB_EP_HIGH_LEVEL,
      ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
      ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
      "high-float");
  delay(SKIMMERSENSE_BETWEEN_REPORTS_MS);

  if (snapshot.batteryValid) {
    reportsOk &= sendSafeReport(
        ZB_EP_TEMPERATURE,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
        "battery");
  } else {
    Serial.println("Report battery     : skipped (MAX17048 unavailable)");
  }

  Serial.printf("Post-report wait: %lu ms...\n",
                static_cast<unsigned long>(SKIMMERSENSE_POST_REPORT_WAIT_MS));
  delay(SKIMMERSENSE_POST_REPORT_WAIT_MS);
  Serial.printf("Snapshot/report stage survived. Reports queued: %s\n",
                reportsOk ? "ALL OK" : "PARTIAL/FAILED");

  enterTimerSleep();
}

void loop() {
  delay(1000);
}