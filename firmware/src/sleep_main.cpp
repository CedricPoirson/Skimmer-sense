#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "driver/rtc_io.h"
#include "soc/soc_caps.h"
#include "skm_radio.h"

#ifndef ZIGBEE_MODE_ED
#error "SkimmerSense must be built in Zigbee End Device mode"
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP_MODE_PER_PIN
#error "SkimmerSense deep-sleep build requires EXT1 per-pin wake levels"
#endif

#ifndef SKIMMERSENSE_NORMAL_TIMER_SECONDS
#define SKIMMERSENSE_NORMAL_TIMER_SECONDS 60ULL
#endif

#ifndef SKIMMERSENSE_LOW_CONFIRM_SECONDS
#define SKIMMERSENSE_LOW_CONFIRM_SECONDS 30ULL
#endif

#ifndef SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS
#define SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS 60ULL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_BEGIN_TIMEOUT_MS
#define SKIMMERSENSE_ZIGBEE_BEGIN_TIMEOUT_MS 30000UL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_WAIT_MS
#define SKIMMERSENSE_ZIGBEE_WAIT_MS 30000UL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_CHANNEL
#define SKIMMERSENSE_ZIGBEE_CHANNEL 20
#endif

#ifndef SKIMMERSENSE_ZIGBEE_TX_POWER_DBM
#define SKIMMERSENSE_ZIGBEE_TX_POWER_DBM 20
#endif

#ifndef SKIMMERSENSE_ZIGBEE_RETRY_SECONDS
#define SKIMMERSENSE_ZIGBEE_RETRY_SECONDS 300ULL
#endif

#ifndef SKIMMERSENSE_ACTIVE_WATCHDOG_MS
#define SKIMMERSENSE_ACTIVE_WATCHDOG_MS 120000UL
#endif

#ifndef SKIMMERSENSE_MAX17048_ALERT_PERCENT
#define SKIMMERSENSE_MAX17048_ALERT_PERCENT 15U
#endif

#ifndef SKIMMERSENSE_MAX17048_LOW_VOLTAGE_MV
#define SKIMMERSENSE_MAX17048_LOW_VOLTAGE_MV 3400U
#endif

static_assert(SKIMMERSENSE_MAX17048_ALERT_PERCENT >= 1U &&
              SKIMMERSENSE_MAX17048_ALERT_PERCENT <= 32U,
              "MAX17048 SOC alert must be between 1 and 32 percent");
static_assert(SKIMMERSENSE_MAX17048_LOW_VOLTAGE_MV <= 5100U,
              "MAX17048 voltage alert must be at most 5.1 V");

static_assert(SKIMMERSENSE_ZIGBEE_CHANNEL >= 11 &&
              SKIMMERSENSE_ZIGBEE_CHANNEL <= 26,
              "Zigbee channel must be between 11 and 26");

#ifndef SKIMMERSENSE_SERIAL_STARTUP_MS
#define SKIMMERSENSE_SERIAL_STARTUP_MS 1200UL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_IDLE_MS
#define SKIMMERSENSE_ZIGBEE_IDLE_MS 8000UL
#endif

#ifndef SKIMMERSENSE_BETWEEN_REPORTS_MS
#define SKIMMERSENSE_BETWEEN_REPORTS_MS 400UL
#endif

#ifndef SKIMMERSENSE_POST_REPORT_WAIT_MS
#define SKIMMERSENSE_POST_REPORT_WAIT_MS 2000UL
#endif

#ifndef SKIMMERSENSE_CRITICAL_FINAL_REPORT_COPIES
#define SKIMMERSENSE_CRITICAL_FINAL_REPORT_COPIES 3U
#endif

#ifndef SKIMMERSENSE_CRITICAL_FINAL_REPORT_GAP_MS
#define SKIMMERSENSE_CRITICAL_FINAL_REPORT_GAP_MS 1000UL
#endif

#ifndef SKIMMERSENSE_BATTERY_REPORT_COPIES
#define SKIMMERSENSE_BATTERY_REPORT_COPIES 2U
#endif

#ifndef SKIMMERSENSE_BATTERY_REPORT_GAP_MS
#define SKIMMERSENSE_BATTERY_REPORT_GAP_MS 250UL
#endif

static_assert(SKIMMERSENSE_BATTERY_REPORT_COPIES >= 1U &&
              SKIMMERSENSE_BATTERY_REPORT_COPIES <= 3U,
              "Battery report copies must be between 1 and 3");

#ifndef SKIMMERSENSE_COLD_BOOT_INTERVIEW_GRACE_MS
#define SKIMMERSENSE_COLD_BOOT_INTERVIEW_GRACE_MS 30000UL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_INTERVIEW_POLL_MS
#define SKIMMERSENSE_ZIGBEE_INTERVIEW_POLL_MS 500UL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_NORMAL_POLL_MS
#define SKIMMERSENSE_ZIGBEE_NORMAL_POLL_MS 10000UL
#endif

#include "Zigbee.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#ifdef SKIMMERSENSE_PRODUCTION_BUILD
static constexpr char FIRMWARE_VERSION[] = "0.9.16-production";
static constexpr char FIRMWARE_FLAVOR[] = "Production anti-wave RTC state machine";
#else
static constexpr char FIRMWARE_VERSION[] = "0.9.16-antiwave";
static constexpr char FIRMWARE_FLAVOR[] = "Anti-wave RTC state machine test";
#endif

static constexpr uint8_t PIN_FLOAT_LOW = D0;
static constexpr uint8_t PIN_FLOAT_HIGH = D1;
static constexpr uint8_t PIN_DS18B20_POWER = D2;
static constexpr uint8_t PIN_DS18B20_DATA = D3;
static constexpr uint8_t PIN_I2C_SDA = D4;
static constexpr uint8_t PIN_I2C_SCL = D5;
static constexpr uint8_t PIN_MAX17048_INT = 4;

static constexpr uint8_t MAX17048_I2C_ADDRESS = 0x36;
static constexpr uint8_t MAX17048_REG_VCELL = 0x02;
static constexpr uint8_t MAX17048_REG_SOC = 0x04;
static constexpr uint8_t MAX17048_REG_VERSION = 0x08;
static constexpr uint8_t MAX17048_REG_CONFIG = 0x0C;
static constexpr uint8_t MAX17048_REG_VALRT = 0x14;
static constexpr uint8_t MAX17048_REG_CRATE = 0x16;
static constexpr uint8_t MAX17048_REG_STATUS = 0x1A;
static constexpr uint16_t MAX17048_CONFIG_ALRT = 0x0020;
static constexpr uint8_t MAX17048_STATUS_RI = 0x01;
static constexpr uint8_t MAX17048_STATUS_VH = 0x02;
static constexpr uint8_t MAX17048_STATUS_VL = 0x04;
static constexpr uint8_t MAX17048_STATUS_VR = 0x08;
static constexpr uint8_t MAX17048_STATUS_HD = 0x10;
static constexpr uint8_t MAX17048_STATUS_SC = 0x20;
static constexpr float MAX17048_CRATE_LSB_PERCENT_PER_HOUR = 0.208f;

static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;
static constexpr uint8_t ZB_EP_SENSOR_FAULT = 13;

struct ZigbeeReportConfirmation {
  volatile bool expected = false;
  volatile bool received = false;
  volatile esp_err_t status = ESP_FAIL;
  volatile uint8_t tsn = 0;
};

ZigbeeReportConfirmation zbReportConfirmations[4];

int reportConfirmationIndex(uint8_t endpoint) {
  if (endpoint == ZB_EP_TEMPERATURE) return 0;
  if (endpoint == ZB_EP_LOW_LEVEL) return 1;
  if (endpoint == ZB_EP_HIGH_LEVEL) return 2;
  if (endpoint == ZB_EP_SENSOR_FAULT) return 3;
  return -1;
}

const char *reportConfirmationLabel(size_t index) {
  static constexpr const char *LABELS[] = {
      "temperature", "low-float", "high-float", "sensor-fault"};
  return index < 4 ? LABELS[index] : "unknown";
}

void zigbeeCommandSendStatusCallback(
    esp_zb_zcl_command_send_status_message_t message) {
  const int index = reportConfirmationIndex(message.src_endpoint);
  if (index < 0) return;
  zbReportConfirmations[index].status = message.status;
  zbReportConfirmations[index].tsn = message.tsn;
  zbReportConfirmations[index].received = true;
}

void resetZigbeeReportConfirmations() {
  for (auto &confirmation : zbReportConfirmations) {
    confirmation.expected = false;
    confirmation.received = false;
    confirmation.status = ESP_FAIL;
    confirmation.tsn = 0;
  }
}

void expectZigbeeReportConfirmation(uint8_t endpoint) {
  const int index = reportConfirmationIndex(endpoint);
  if (index < 0) return;
  zbReportConfirmations[index].expected = true;
  zbReportConfirmations[index].received = false;
  zbReportConfirmations[index].status = ESP_FAIL;
  zbReportConfirmations[index].tsn = 0;
}

bool logZigbeeReportConfirmationFailures() {
  bool explicitFailureDetected = false;
  for (size_t index = 0; index < 4; ++index) {
    const ZigbeeReportConfirmation &confirmation = zbReportConfirmations[index];
    if (!confirmation.expected) continue;
    if (!confirmation.received) {
      Serial.printf("Report %-12s: delivery confirmation unavailable (stack callback not emitted)\n",
                    reportConfirmationLabel(index));
      skmCycleLogAppend("Report %s: delivery confirmation unavailable (stack callback not emitted)",
                        reportConfirmationLabel(index));
      continue;
    }
    const bool ok = confirmation.status == ESP_OK;
    explicitFailureDetected |= !ok;
    Serial.printf("Report %-12s: delivery %s / TSN %u / 0x%x (%s)\n",
                  reportConfirmationLabel(index),
                  ok ? "CONFIRMED" : "FAILED",
                  static_cast<unsigned>(confirmation.tsn),
                  confirmation.status,
                  esp_err_to_name(confirmation.status));
  }
  return explicitFailureDetected;
}

static constexpr uint32_t RTC_MAGIC = 0x534B4D32UL;

enum class LevelState : uint8_t {
  NORMAL = 0,
  LOW_PENDING = 1,
  WAIT_HIGH = 2,
};

RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR uint8_t rtcStateRaw = static_cast<uint8_t>(LevelState::NORMAL);
RTC_DATA_ATTR bool rtcFinalReportPending = false;
RTC_DATA_ATTR bool rtcFinalLowClosed = false;
RTC_DATA_ATTR bool rtcFinalHighClosed = false;
RTC_DATA_ATTR bool rtcFaultReportKnown = false;
RTC_DATA_ATTR bool rtcLastFaultReported = false;
RTC_DATA_ATTR uint64_t rtcNormalSleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
RTC_DATA_ATTR float rtcLastWaterTemperatureC = 20.0f;
RTC_DATA_ATTR bool rtcNormalSleepValid = false;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

class PreloadBinary : public ZigbeeBinary {
 public:
  explicit PreloadBinary(uint8_t endpoint) : ZigbeeBinary(endpoint) {}
  bool preloadBinaryInput(bool value) {
    esp_zb_attribute_list_t *cluster = esp_zb_cluster_list_get_cluster(
        _cluster_list,
        ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (cluster == nullptr) return false;
    return esp_zb_cluster_update_attr(
               cluster,
               ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
               &value) == ESP_OK;
  }
};

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
PreloadBinary zbLowLevel(ZB_EP_LOW_LEVEL);
PreloadBinary zbHighLevel(ZB_EP_HIGH_LEVEL);
PreloadBinary zbSensorFault(ZB_EP_SENSOR_FAULT);

struct SensorSnapshot {
  bool lowClosed = false;
  bool highClosed = false;
  bool maxIntLow = false;
  bool batteryValid = false;
  uint16_t maxVersion = 0;
  float batteryVoltage = 0.0f;
  float batterySocRaw = 0.0f;
  uint8_t batteryPercent = 100;
  uint8_t batteryVoltageZcl = 40;
  bool batteryRateValid = false;
  float batteryRatePercentPerHour = 0.0f;
  uint8_t batteryStatus = 0;
  bool batteryResetDetected = false;
  bool temperatureValid = false;
  float waterTemperatureC = 20.0f;
};

struct CyclePlan {
  LevelState nextState = LevelState::NORMAL;
  bool useZigbee = false;
  bool reportTemperature = false;
  bool reportFloats = false;
  bool reportFault = false;
  uint64_t sleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
  bool watchLow = false;
  bool watchHigh = false;
  bool watchMax = false;
  const char *reason = "normal";
};

const char *stateName(LevelState state) {
  switch (state) {
    case LevelState::NORMAL: return "NORMAL";
    case LevelState::LOW_PENDING: return "LOW_PENDING";
    case LevelState::WAIT_HIGH: return "WAIT_HIGH";
    default: return "UNKNOWN";
  }
}

const char *contactState(bool closed) { return closed ? "CLOSED" : "OPEN"; }

uint64_t currentExt1Mask() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return 0;
  return esp_sleep_get_ext1_wakeup_status();
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
  value = (static_cast<uint16_t>(Wire.read()) << 8) | static_cast<uint16_t>(Wire.read());
  return true;
}

bool writeRegister16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool configureMax17048Alerts() {
  uint16_t config = 0, voltageAlert = 0;
  if (!readRegister16(MAX17048_REG_CONFIG, config) ||
      !readRegister16(MAX17048_REG_VALRT, voltageAlert)) return false;
  const uint16_t socThreshold = static_cast<uint16_t>(32U - SKIMMERSENSE_MAX17048_ALERT_PERCENT);
  const uint16_t wantedConfig = static_cast<uint16_t>((config & 0xFFE0U) | socThreshold);
  const uint16_t lowVoltageUnits = static_cast<uint16_t>((SKIMMERSENSE_MAX17048_LOW_VOLTAGE_MV + 10U) / 20U);
  const uint16_t wantedVoltageAlert = static_cast<uint16_t>((lowVoltageUnits << 8) | (voltageAlert & 0x00FFU));
  bool ok = true;
  if (wantedConfig != config) ok &= writeRegister16(MAX17048_REG_CONFIG, wantedConfig);
  if (wantedVoltageAlert != voltageAlert) ok &= writeRegister16(MAX17048_REG_VALRT, wantedVoltageAlert);
  return ok;
}

void acknowledgeMax17048Alert() {
  uint16_t rawStatus = 0, config = 0;
  if (!readRegister16(MAX17048_REG_STATUS, rawStatus) ||
      !readRegister16(MAX17048_REG_CONFIG, config)) return;
  const uint8_t alertFlags = static_cast<uint8_t>(
      MAX17048_STATUS_SC | MAX17048_STATUS_HD | MAX17048_STATUS_VR |
      MAX17048_STATUS_VL | MAX17048_STATUS_VH | MAX17048_STATUS_RI);
  writeRegister16(MAX17048_REG_STATUS,
                  rawStatus & ~(static_cast<uint16_t>(alertFlags) << 8));
  if (config & MAX17048_CONFIG_ALRT)
    writeRegister16(MAX17048_REG_CONFIG, config & ~MAX17048_CONFIG_ALRT);
}

float readWaterTemperatureC() {
  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, HIGH);
  delay(20);
  temperatureSensors.begin();
  if (temperatureSensors.getDeviceCount() == 0) return DEVICE_DISCONNECTED_C;
  temperatureSensors.setResolution(10);
  temperatureSensors.requestTemperatures();
  const float value = temperatureSensors.getTempCByIndex(0);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  return value;
}

SensorSnapshot readBaseSensorSnapshot(bool manageAlerts = true) {
  SensorSnapshot snapshot;
  releaseRtcWakePins();
  pinMode(PIN_FLOAT_LOW, INPUT_PULLUP);
  pinMode(PIN_FLOAT_HIGH, INPUT_PULLUP);
  pinMode(PIN_MAX17048_INT, INPUT_PULLUP);
  snapshot.lowClosed = digitalRead(PIN_FLOAT_LOW) == LOW;
  snapshot.highClosed = digitalRead(PIN_FLOAT_HIGH) == LOW;
  snapshot.maxIntLow = digitalRead(PIN_MAX17048_INT) == LOW;
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  uint16_t rawVcell = 0, rawSoc = 0, rawCrate = 0, rawStatus = 0, version = 0;
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  if (Wire.endTransmission() == 0 &&
      readRegister16(MAX17048_REG_VERSION, version) &&
      readRegister16(MAX17048_REG_VCELL, rawVcell) &&
      readRegister16(MAX17048_REG_SOC, rawSoc)) {
    snapshot.batteryValid = true;
    snapshot.maxVersion = version;
    snapshot.batteryVoltage = static_cast<float>(rawVcell) * 78.125f / 1000000.0f;
    snapshot.batterySocRaw = static_cast<float>(rawSoc) / 256.0f;
    float clampedSoc = constrain(snapshot.batterySocRaw, 0.0f, 100.0f);
    snapshot.batteryPercent = static_cast<uint8_t>(clampedSoc + 0.5f);
    snapshot.batteryVoltageZcl = static_cast<uint8_t>(constrain(static_cast<int>(snapshot.batteryVoltage * 10.0f + 0.5f), 0, 255));
    if (readRegister16(MAX17048_REG_CRATE, rawCrate)) {
      snapshot.batteryRateValid = true;
      snapshot.batteryRatePercentPerHour = static_cast<float>(static_cast<int16_t>(rawCrate)) * MAX17048_CRATE_LSB_PERCENT_PER_HOUR;
    }
    if (readRegister16(MAX17048_REG_STATUS, rawStatus)) {
      snapshot.batteryStatus = static_cast<uint8_t>(rawStatus >> 8);
      snapshot.batteryResetDetected = (snapshot.batteryStatus & MAX17048_STATUS_RI) != 0;
    }
  }
  if (snapshot.batteryValid && manageAlerts) {
    configureMax17048Alerts();
    acknowledgeMax17048Alert();
  }
  return snapshot;
}

void readTemperatureIntoSnapshot(SensorSnapshot &snapshot) {
  const float temperature = readWaterTemperatureC();
  snapshot.temperatureValid = temperature != DEVICE_DISCONNECTED_C &&
                              temperature > -55.0f && temperature < 85.0f;
  if (snapshot.temperatureValid) snapshot.waterTemperatureC = temperature;
}

bool configureZigbeeEndpoints(const SensorSnapshot &snapshot) {
  bool ok = true;
  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(snapshot.waterTemperatureC);
  zbTemperature.setTolerance(1);
  if (snapshot.batteryValid) {
    ok &= zbTemperature.setPowerSource(ZB_POWER_SOURCE_BATTERY,
                                       snapshot.batteryPercent,
                                       snapshot.batteryVoltageZcl);
  }
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
  const bool sensorFault = snapshot.lowClosed && !snapshot.highClosed;
  zbSensorFault.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  ok &= zbSensorFault.addBinaryInput();
  ok &= zbSensorFault.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  ok &= zbSensorFault.setBinaryInputDescription("Sensor Fault");
  ok &= zbSensorFault.preloadBinaryInput(sensorFault);
  ok &= Zigbee.addEndpoint(&zbTemperature);
  ok &= Zigbee.addEndpoint(&zbLowLevel);
  ok &= Zigbee.addEndpoint(&zbHighLevel);
  ok &= Zigbee.addEndpoint(&zbSensorFault);
  return ok;
}

bool startZigbee(bool) {
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_2048MIN;
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = SKIMMERSENSE_ZIGBEE_NORMAL_POLL_MS;
  Zigbee.setTimeout(SKIMMERSENSE_ZIGBEE_BEGIN_TIMEOUT_MS);
  Zigbee.setPrimaryChannelMask(1UL << SKIMMERSENSE_ZIGBEE_CHANNEL);
  if (!Zigbee.begin(&zigbeeConfig, false)) return false;
  const uint32_t startedAt = millis();
  while (!Zigbee.connected() && millis() - startedAt < SKIMMERSENSE_ZIGBEE_WAIT_MS) delay(100);
  if (!Zigbee.connected()) return false;
  resetZigbeeReportConfirmations();
  esp_zb_zcl_command_send_status_handler_register(zigbeeCommandSendStatusCallback);
  return true;
}

bool sendSafeReport(uint8_t endpoint, uint16_t clusterId, uint16_t attributeId, const char *label) {
  esp_zb_zcl_report_attr_cmd_t report{};
  report.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  report.attributeID = attributeId;
  report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report.clusterID = clusterId;
  report.zcl_basic_cmd.src_endpoint = endpoint;
  Serial.printf("Report %-12s: queue...\n", label);
  expectZigbeeReportConfirmation(endpoint);
  if (!esp_zb_lock_acquire(portMAX_DELAY)) return false;
  const esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report);
  esp_zb_lock_release();
  if (err != ESP_OK) return false;
  Serial.printf("Report %-12s: queued OK\n", label);
  return true;
}

bool armOppositeLevelWake(uint8_t pin, bool currentHigh, const char *label) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  if (!esp_sleep_is_valid_wakeup_gpio(gpio)) return false;
  rtc_gpio_init(gpio);
  rtc_gpio_pulldown_dis(gpio);
  rtc_gpio_pullup_en(gpio);
  const esp_sleep_ext1_wakeup_mode_t mode =
      currentHigh ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH;
  const esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << pin, mode);
  if (err != ESP_OK) return false;
  Serial.printf("Wake %s GPIO%u: %s -> wake on %s\n", label, pin,
                currentHigh ? "HIGH" : "LOW",
                currentHigh ? "LOW" : "HIGH");
  return true;
}

LevelState loadState(esp_sleep_wakeup_cause_t cause) {
  const bool rtcInvalid = rtcMagic != RTC_MAGIC ||
      rtcStateRaw > static_cast<uint8_t>(LevelState::WAIT_HIGH);
  if (rtcInvalid) {
    rtcMagic = RTC_MAGIC;
    rtcStateRaw = static_cast<uint8_t>(LevelState::NORMAL);
    rtcFinalReportPending = false;
    rtcFaultReportKnown = false;
    rtcLastFaultReported = false;
    return LevelState::NORMAL;
  }
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    rtcStateRaw = static_cast<uint8_t>(LevelState::NORMAL);
    return LevelState::NORMAL;
  }
  return static_cast<LevelState>(rtcStateRaw);
}

CyclePlan makePlan(LevelState state,
                   const SensorSnapshot &snapshot,
                   esp_sleep_wakeup_cause_t cause,
                   uint64_t extMask) {
  CyclePlan plan;
  const bool timerWake = cause == ESP_SLEEP_WAKEUP_TIMER;
  const bool coldBoot = cause == ESP_SLEEP_WAKEUP_UNDEFINED;
  const bool maxWake = (extMask & (1ULL << PIN_MAX17048_INT)) != 0;
  switch (state) {
    case LevelState::NORMAL:
      if (snapshot.lowClosed && !snapshot.highClosed) {
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFault = true;
        plan.watchLow = true;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.reason = "IMPOSSIBLE LOW=CLOSED HIGH=OPEN -> publish SENSOR FAULT only";
      } else if (snapshot.lowClosed) {
        plan.nextState = LevelState::LOW_PENDING;
        plan.sleepSeconds = SKIMMERSENSE_LOW_CONFIRM_SECONDS;
        plan.watchLow = true;
        plan.reason = "LOW closed -> continuous confirmation; wake if LOW reopens";
      } else {
        plan.watchLow = true;
        plan.watchMax = true;
        plan.useZigbee = coldBoot || timerWake;
        plan.reportTemperature = plan.useZigbee;
        plan.reportFloats = plan.useZigbee;
        if (maxWake) {
          plan.useZigbee = true;
          plan.reportTemperature = false;
          plan.reportFloats = false;
        }
      }
      break;
    case LevelState::LOW_PENDING:
      if (!snapshot.lowClosed) {
        plan.watchLow = true;
        plan.watchMax = true;
        plan.reason = "LOW reopened during confirmation -> rejected as wave/bather motion";
      } else if (!timerWake) {
        plan.nextState = LevelState::LOW_PENDING;
        plan.sleepSeconds = SKIMMERSENSE_LOW_CONFIRM_SECONDS;
        plan.watchLow = true;
      } else if (!snapshot.highClosed) {
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFault = true;
        plan.watchLow = true;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.reason = "IMPOSSIBLE LOW=CLOSED HIGH=OPEN after confirmation -> publish SENSOR FAULT only";
      } else {
        plan.nextState = LevelState::WAIT_HIGH;
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFloats = true;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.sleepSeconds = SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS;
        plan.reason = "LOW continuously confirmed -> publish ON/ON, then ignore LOW and watch HIGH";
      }
      break;
    case LevelState::WAIT_HIGH:
      if (!snapshot.highClosed) {
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFloats = true;
        plan.watchLow = true;
        plan.watchMax = true;
        plan.reason = "HIGH opened -> publish final float states and return NORMAL";
      } else {
        plan.nextState = LevelState::WAIT_HIGH;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.sleepSeconds = SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS;
        if (timerWake) {
          plan.useZigbee = true;
          plan.reportTemperature = true;
        }
        if (maxWake) {
          plan.useZigbee = true;
          plan.reportTemperature = false;
        }
      }
      break;
  }
  if (coldBoot && !plan.useZigbee) {
    plan.useZigbee = true;
    plan.reportTemperature = true;
  }
  const bool sensorFault = snapshot.lowClosed && !snapshot.highClosed;
  if (plan.useZigbee) plan.reportFault = true;
  if (rtcFaultReportKnown && rtcLastFaultReported && !sensorFault && !plan.useZigbee) {
    plan.useZigbee = true;
    plan.reportFault = true;
    plan.reportTemperature = false;
    plan.reportFloats = false;
    plan.reason = "sensor state coherent again -> immediate SENSOR FAULT OFF";
  }
  return plan;
}

void applyPendingFinalReport(CyclePlan &plan) {
  if (!rtcFinalReportPending) return;
  plan.nextState = LevelState::NORMAL;
  plan.useZigbee = true;
  plan.reportFloats = true;
  plan.reportFault = true;
  plan.watchLow = true;
  plan.watchMax = true;
  plan.reason = "pending refill-completion snapshot -> retry three LOW/HIGH copies";
}

[[noreturn]] void enterPlannedSleep(CyclePlan plan) {
  rtcMagic = RTC_MAGIC;
  rtcStateRaw = static_cast<uint8_t>(plan.nextState);
  esp_sleep_disable_ext1_wakeup_io(0);
  if (plan.watchLow) armOppositeLevelWake(PIN_FLOAT_LOW, digitalRead(PIN_FLOAT_LOW) == HIGH, "LOW-float");
  if (plan.watchHigh) armOppositeLevelWake(PIN_FLOAT_HIGH, digitalRead(PIN_FLOAT_HIGH) == HIGH, "HIGH-float");
  if (plan.watchMax) armOppositeLevelWake(PIN_MAX17048_INT, digitalRead(PIN_MAX17048_INT) == HIGH, "MAX17048-ALRT");
  esp_sleep_enable_timer_wakeup(plan.sleepSeconds * 1000000ULL);
  Serial.printf("Next state: %s\n", stateName(plan.nextState));
  Serial.printf("Deep sleep: %llu s\n", static_cast<unsigned long long>(plan.sleepSeconds));
  Serial.flush();
  delay(20);
  esp_deep_sleep_start();
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  skmSelectRadioAntenna();
  delay(SKIMMERSENSE_SERIAL_STARTUP_MS);
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const uint64_t extMask = currentExt1Mask();
  const LevelState state = loadState(cause);
  SensorSnapshot snapshot = readBaseSensorSnapshot();
  if (state == LevelState::WAIT_HIGH && !snapshot.highClosed) {
    rtcFinalReportPending = true;
    rtcFinalLowClosed = snapshot.lowClosed;
    rtcFinalHighClosed = snapshot.highClosed;
  }
  CyclePlan plan = makePlan(state, snapshot, cause, extMask);
  applyPendingFinalReport(plan);
  const bool temperatureReadRequested = plan.reportTemperature;
  if (temperatureReadRequested) {
    readTemperatureIntoSnapshot(snapshot);
    plan = makePlan(state, snapshot, cause, extMask);
    applyPendingFinalReport(plan);
  }
  Serial.printf("Float LOW : %s\n", contactState(snapshot.lowClosed));
  Serial.printf("Float HIGH: %s\n", contactState(snapshot.highClosed));
  Serial.printf("Decision: %s\n", plan.reason);
  SensorSnapshot zigbeeSnapshot = snapshot;
  if (rtcFinalReportPending) {
    zigbeeSnapshot.lowClosed = rtcFinalLowClosed;
    zigbeeSnapshot.highClosed = rtcFinalHighClosed;
  }
  if (plan.useZigbee && configureZigbeeEndpoints(zigbeeSnapshot) && startZigbee(cause == ESP_SLEEP_WAKEUP_UNDEFINED)) {
    delay(SKIMMERSENSE_ZIGBEE_IDLE_MS);
    bool reportsOk = true;
    if (plan.reportTemperature && snapshot.temperatureValid) {
      reportsOk &= sendSafeReport(ZB_EP_TEMPERATURE,
                                  ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                                  ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                                  "temperature");
      delay(SKIMMERSENSE_BETWEEN_REPORTS_MS);
    }
    if (plan.reportFault) {
      reportsOk &= sendSafeReport(ZB_EP_SENSOR_FAULT,
                                  ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
                                  ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
                                  "sensor-fault");
      delay(SKIMMERSENSE_BETWEEN_REPORTS_MS);
    }
    if (plan.reportFloats) {
      const bool critical = rtcFinalReportPending;
      const uint8_t copies = critical ? SKIMMERSENSE_CRITICAL_FINAL_REPORT_COPIES : 1U;
      for (uint8_t copy = 0; copy < copies; ++copy) {
        reportsOk &= sendSafeReport(ZB_EP_LOW_LEVEL,
                                    ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
                                    ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
                                    "low-float");
        delay(SKIMMERSENSE_BETWEEN_REPORTS_MS);
        reportsOk &= sendSafeReport(ZB_EP_HIGH_LEVEL,
                                    ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
                                    ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
                                    "high-float");
        if (copy + 1U < copies) delay(SKIMMERSENSE_CRITICAL_FINAL_REPORT_GAP_MS);
      }
    }
    delay(SKIMMERSENSE_POST_REPORT_WAIT_MS);
    const bool explicitDeliveryFailure = logZigbeeReportConfirmationFailures();
    if (plan.reportFault && reportsOk && !explicitDeliveryFailure) {
      rtcFaultReportKnown = true;
      rtcLastFaultReported = zigbeeSnapshot.lowClosed && !zigbeeSnapshot.highClosed;
      Serial.printf("Sensor fault state accepted by Zigbee stack: %s\n",
                    rtcLastFaultReported ? "ON" : "OFF");
    }
    if (rtcFinalReportPending && reportsOk && !explicitDeliveryFailure) {
      rtcFinalReportPending = false;
    }
  }
  enterPlannedSleep(plan);
}

void loop() { delay(1000); }
