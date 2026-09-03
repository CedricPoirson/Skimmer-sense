#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_sleep.h"
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

#include "Zigbee.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#ifdef SKIMMERSENSE_PRODUCTION_BUILD
static constexpr char FIRMWARE_VERSION[] = "0.9-production";
static constexpr char FIRMWARE_FLAVOR[] = "Production anti-wave RTC state machine";
#else
static constexpr char FIRMWARE_VERSION[] = "0.9-deepsleep-zigbee-antiwave";
static constexpr char FIRMWARE_FLAVOR[] = "Anti-wave RTC state machine test";
#endif

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
static constexpr uint8_t MAX17048_REG_CONFIG = 0x0C;
static constexpr uint8_t MAX17048_REG_STATUS = 0x1A;
static constexpr uint16_t MAX17048_CONFIG_ALRT = 0x0020;
static constexpr uint8_t MAX17048_STATUS_RI = 0x01;

static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;

struct ZigbeeReportConfirmation {
  volatile bool expected = false;
  volatile bool received = false;
  volatile esp_err_t status = ESP_FAIL;
  volatile uint8_t tsn = 0;
};

ZigbeeReportConfirmation zbReportConfirmations[3];

int reportConfirmationIndex(uint8_t endpoint) {
  if (endpoint == ZB_EP_TEMPERATURE) return 0;
  if (endpoint == ZB_EP_LOW_LEVEL) return 1;
  if (endpoint == ZB_EP_HIGH_LEVEL) return 2;
  return -1;
}

const char *reportConfirmationLabel(size_t index) {
  static constexpr const char *LABELS[] = {
      "temperature", "low-float", "high-float"};
  return index < 3 ? LABELS[index] : "unknown";
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

bool logZigbeeReportConfirmations() {
  bool allConfirmed = true;
  bool anyExpected = false;

  for (size_t index = 0; index < 3; ++index) {
    const ZigbeeReportConfirmation &confirmation =
        zbReportConfirmations[index];
    if (!confirmation.expected) continue;
    anyExpected = true;

    if (!confirmation.received) {
      allConfirmed = false;
      Serial.printf("Report %-12s: delivery confirmation NOT RECEIVED\n",
                    reportConfirmationLabel(index));
      skmCycleLogAppend("Report %s: delivery confirmation NOT RECEIVED",
                        reportConfirmationLabel(index));
      continue;
    }

    const esp_err_t status = confirmation.status;
    const bool ok = status == ESP_OK;
    allConfirmed &= ok;
    Serial.printf("Report %-12s: delivery %s / TSN %u / 0x%x (%s)\n",
                  reportConfirmationLabel(index),
                  ok ? "CONFIRMED" : "FAILED",
                  static_cast<unsigned>(confirmation.tsn),
                  status,
                  esp_err_to_name(status));
    skmCycleLogAppend("Report %s: delivery %s / TSN %u / 0x%x (%s)",
                      reportConfirmationLabel(index),
                      ok ? "CONFIRMED" : "FAILED",
                      static_cast<unsigned>(confirmation.tsn),
                      status,
                      esp_err_to_name(status));
  }

  return anyExpected && allConfirmed;
}

static constexpr uint32_t RTC_MAGIC = 0x534B4D31UL;  // "SKM1"

enum class LevelState : uint8_t {
  NORMAL = 0,
  LOW_PENDING = 1,
  WAIT_HIGH = 2,
};

RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR uint8_t rtcStateRaw = static_cast<uint8_t>(LevelState::NORMAL);

// Last valid adaptive NORMAL interval retained across deep sleep.
// Short event-only wakes can therefore skip the DS18B20 conversion.
RTC_DATA_ATTR uint64_t rtcNormalSleepSeconds =
    SKIMMERSENSE_NORMAL_TIMER_SECONDS;
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
  bool maxIntLow = false;
  bool batteryValid = false;
  uint16_t maxVersion = 0;
  float batteryVoltage = 0.0f;
  float batterySocRaw = 0.0f;
  uint8_t batteryPercent = 100;
  uint8_t batteryVoltageZcl = 40;
  bool temperatureValid = false;
  float waterTemperatureC = 20.0f;
};

struct CyclePlan {
  LevelState nextState = LevelState::NORMAL;
  bool useZigbee = false;
  bool reportTemperature = false;
  bool reportFloats = false;
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

const char *contactState(bool closed) {
  return closed ? "CLOSED" : "OPEN";
}


uint64_t normalSleepSecondsForTemperature(
    const SensorSnapshot &snapshot) {
#ifndef SKIMMERSENSE_PRODUCTION_BUILD
  return SKIMMERSENSE_NORMAL_TIMER_SECONDS;
#else
  if (!snapshot.temperatureValid) {
    return rtcNormalSleepValid
             ? rtcNormalSleepSeconds
             : SKIMMERSENSE_NORMAL_TIMER_SECONDS;
  }

  const float t = snapshot.waterTemperatureC;
  uint64_t seconds;

  if (t >= 28.0f)      seconds = 1800ULL;
  else if (t >= 24.0f) seconds = 3600ULL;
  else if (t >= 18.0f) seconds = 7200ULL;
  else if (t >= 12.0f) seconds = 14400ULL;
  else if (t >= 5.0f)  seconds = 21600ULL;
  else if (t >= 3.0f)  seconds = 7200ULL;
  else                 seconds = 1800ULL;

  rtcNormalSleepSeconds = seconds;
  rtcLastWaterTemperatureC = t;
  rtcNormalSleepValid = true;

  return seconds;
#endif
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

uint64_t currentExt1Mask() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
    return 0;
  }
  return esp_sleep_get_ext1_wakeup_status();
}

void printWakeReason() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause: %s\n", wakeCauseName(cause));
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    const uint64_t mask = currentExt1Mask();
    Serial.printf("EXT1 wake mask: 0x%llX", static_cast<unsigned long long>(mask));
    if (mask & (1ULL << PIN_FLOAT_LOW)) Serial.print(" LOW-float");
    if (mask & (1ULL << PIN_FLOAT_HIGH)) Serial.print(" HIGH-float");
    if (mask & (1ULL << PIN_MAX17048_INT)) Serial.print(" MAX17048-ALRT");
    Serial.println();
  }
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

bool writeRegister16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

void acknowledgeMax17048Alert() {
  uint16_t rawStatus = 0;
  uint16_t config = 0;
  if (!readRegister16(MAX17048_REG_STATUS, rawStatus) ||
      !readRegister16(MAX17048_REG_CONFIG, config)) {
    return;
  }

  const uint8_t status = static_cast<uint8_t>(rawStatus >> 8);
  if (status & MAX17048_STATUS_RI) {
    writeRegister16(MAX17048_REG_STATUS,
                    rawStatus & ~(static_cast<uint16_t>(MAX17048_STATUS_RI) << 8));
  }
  if (config & MAX17048_CONFIG_ALRT) {
    writeRegister16(MAX17048_REG_CONFIG, config & ~MAX17048_CONFIG_ALRT);
  }
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

SensorSnapshot readBaseSensorSnapshot() {
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
  snapshot.maxIntLow = digitalRead(PIN_MAX17048_INT) == LOW;

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

  acknowledgeMax17048Alert();
  return snapshot;
}

void readTemperatureIntoSnapshot(SensorSnapshot &snapshot) {
  snapshot.temperatureValid = false;

  const float temperature = readWaterTemperatureC();

  if (temperature != DEVICE_DISCONNECTED_C &&
      temperature > -55.0f &&
      temperature < 85.0f) {
    snapshot.temperatureValid = true;
    snapshot.waterTemperatureC = temperature;
  }
}

bool configureZigbeeEndpoints(const SensorSnapshot &snapshot) {
  bool ok = true;

  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(snapshot.waterTemperatureC);
  zbTemperature.setTolerance(1);
// Power Configuration disabled.
// ESP32-C6 Arduino Zigbee stack / ZBOSS crash observed with setPowerSource().
// Battery monitoring remains available through MAX17048.
  ok &= true;

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

void configureZigbeeTxPower() {
  int8_t beforeDbm = 0;
  int8_t afterDbm = 0;

  if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
    Serial.println("Zigbee TX power: lock unavailable; keeping stack default");
    skmCycleLogAppend("Zigbee TX power: lock unavailable; stack default kept");
    return;
  }

  esp_zb_get_tx_power(&beforeDbm);
  if (beforeDbm < SKIMMERSENSE_ZIGBEE_TX_POWER_DBM) {
    esp_zb_set_tx_power(SKIMMERSENSE_ZIGBEE_TX_POWER_DBM);
  }
  esp_zb_get_tx_power(&afterDbm);
  esp_zb_lock_release();

  Serial.printf("Zigbee TX power: %d dBm -> %d dBm%s\n",
                static_cast<int>(beforeDbm),
                static_cast<int>(afterDbm),
                beforeDbm < SKIMMERSENSE_ZIGBEE_TX_POWER_DBM
                    ? " (target applied)"
                    : " (stack value retained)");
  skmCycleLogAppend("Zigbee TX power: %d dBm -> %d dBm%s",
                    static_cast<int>(beforeDbm),
                    static_cast<int>(afterDbm),
                    beforeDbm < SKIMMERSENSE_ZIGBEE_TX_POWER_DBM
                        ? " (target applied)"
                        : " (stack value retained)");
}

bool logZigbeeParentReception(const char *phase) {
  esp_zb_nwk_info_iterator_t iterator = ESP_ZB_NWK_INFO_ITERATOR_INIT;
  esp_zb_nwk_neighbor_info_t neighbor{};
  bool found = false;
  int8_t parentRssiDbm = 0;
  uint8_t parentLqi = 0;
  uint16_t parentShortAddress = 0xFFFF;

  if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
    Serial.printf("Zigbee parent RX (%s): unavailable (stack lock)\n", phase);
    skmCycleLogAppend("Zigbee parent RX (%s): unavailable (stack lock)", phase);
    return false;
  }

  while (esp_zb_nwk_get_next_neighbor(&iterator, &neighbor) == ESP_OK) {
    if (neighbor.relationship == ESP_ZB_NWK_RELATIONSHIP_PARENT) {
      parentRssiDbm = neighbor.rssi;
      parentLqi = neighbor.lqi;
      parentShortAddress = neighbor.short_addr;
      found = true;
      break;
    }
  }
  esp_zb_lock_release();

  if (!found) {
    Serial.printf("Zigbee parent RX (%s): unavailable on this wake\n", phase);
    skmCycleLogAppend("Zigbee parent RX (%s): unavailable on this wake", phase);
    return false;
  }

  Serial.printf("Zigbee parent RX (%s): RSSI %d dBm / LQI %u / short 0x%04X\n",
                phase,
                static_cast<int>(parentRssiDbm),
                static_cast<unsigned>(parentLqi),
                static_cast<unsigned>(parentShortAddress));
  skmCycleLogAppend("Zigbee parent RX (%s): RSSI %d dBm / LQI %u / short 0x%04X",
                    phase,
                    static_cast<int>(parentRssiDbm),
                    static_cast<unsigned>(parentLqi),
                    static_cast<unsigned>(parentShortAddress));
  return true;
}

void scheduleZigbeeRecovery(CyclePlan &plan, const char *failureStage) {
  const uint64_t plannedSleepSeconds = plan.sleepSeconds;
  if (plan.sleepSeconds > SKIMMERSENSE_ZIGBEE_RETRY_SECONDS) {
    plan.sleepSeconds = SKIMMERSENSE_ZIGBEE_RETRY_SECONDS;
  }

  Serial.printf(
      "Zigbee recovery after %s: retry in %llu s (planned interval was %llu s)\n",
      failureStage,
      static_cast<unsigned long long>(plan.sleepSeconds),
      static_cast<unsigned long long>(plannedSleepSeconds));
  skmCycleLogAppend(
      "Zigbee recovery after %s: retry in %llu s (planned interval was %llu s)",
      failureStage,
      static_cast<unsigned long long>(plan.sleepSeconds),
      static_cast<unsigned long long>(plannedSleepSeconds));
}

bool startZigbee() {
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();

  // Adaptive NORMAL sleep can reach 6 hours. Keep the Zigbee child
  // relationship alive much longer than the default sleepy-device timeout.
  zigbeeConfig.nwk_cfg.zed_cfg.ed_timeout =
      ESP_ZB_ED_AGING_TIMEOUT_2048MIN;
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 10000;
  Zigbee.setTimeout(SKIMMERSENSE_ZIGBEE_BEGIN_TIMEOUT_MS);

  const uint32_t primaryChannelMask = 1UL << SKIMMERSENSE_ZIGBEE_CHANNEL;
  Zigbee.setPrimaryChannelMask(primaryChannelMask);
  Serial.printf("Zigbee primary channel: %u (mask 0x%08lX)\n",
                static_cast<unsigned>(SKIMMERSENSE_ZIGBEE_CHANNEL),
                static_cast<unsigned long>(primaryChannelMask));
  skmCycleLogAppend("Zigbee primary channel: %u (mask 0x%08lX)",
                    static_cast<unsigned>(SKIMMERSENSE_ZIGBEE_CHANNEL),
                    static_cast<unsigned long>(primaryChannelMask));

  Serial.println("Starting Zigbee sleepy End Device...");
  skmCycleLogAppend("Zigbee: starting sleepy End Device");
  const uint32_t beginStartedAt = millis();
  if (!Zigbee.begin(&zigbeeConfig, false)) {
    const uint32_t elapsed = millis() - beginStartedAt;
    Serial.printf("Zigbee begin failed after %lu ms\n",
                  static_cast<unsigned long>(elapsed));
    skmCycleLogAppend("Zigbee: begin FAILED after %lu ms",
                      static_cast<unsigned long>(elapsed));
    return false;
  }
  const uint32_t beginElapsed = millis() - beginStartedAt;
  Serial.printf("Zigbee stack started in %lu ms\n",
                static_cast<unsigned long>(beginElapsed));
  skmCycleLogAppend("Zigbee: stack started in %lu ms",
                    static_cast<unsigned long>(beginElapsed));

  // ZBOSS is initialized now; set power before network reconnection.
  configureZigbeeTxPower();

  Serial.print("Waiting for Zigbee network");
  const uint32_t startedAt = millis();
  while (!Zigbee.connected() && millis() - startedAt < SKIMMERSENSE_ZIGBEE_WAIT_MS) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();

  if (!Zigbee.connected()) {
    const uint32_t elapsed = millis() - startedAt;
    Serial.printf("Zigbee reconnect timeout after %lu ms\n",
                  static_cast<unsigned long>(elapsed));
    skmCycleLogAppend("Zigbee: reconnect TIMEOUT after %lu ms",
                      static_cast<unsigned long>(elapsed));
    return false;
  }

  const uint32_t reconnectElapsed = millis() - startedAt;
  Serial.printf("Zigbee connected in %lu ms after stack start!\n",
                static_cast<unsigned long>(reconnectElapsed));
  skmCycleLogAppend("Zigbee: connected in %lu ms after stack start",
                    static_cast<unsigned long>(reconnectElapsed));

  resetZigbeeReportConfirmations();
  esp_zb_zcl_command_send_status_handler_register(
      zigbeeCommandSendStatusCallback);
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
  expectZigbeeReportConfirmation(endpoint);
  if (!esp_zb_lock_acquire(portMAX_DELAY)) {
    Serial.printf("Report %-12s: Zigbee lock FAILED\n", label);
    skmCycleLogAppend("Report %s: Zigbee lock FAILED", label);
    return false;
  }
  const esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report);
  esp_zb_lock_release();

  if (err != ESP_OK) {
    Serial.printf("Report %-12s: FAILED 0x%x (%s)\n", label, err, esp_err_to_name(err));
    skmCycleLogAppend("Report %s: FAILED 0x%x (%s)",
                      label, err, esp_err_to_name(err));
    return false;
  }
  Serial.printf("Report %-12s: queued OK\n", label);
  skmCycleLogAppend("Report %s: queued OK", label);
  return true;
}

bool armOppositeLevelWake(uint8_t pin, bool currentHigh, const char *label) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  if (!esp_sleep_is_valid_wakeup_gpio(gpio)) {
    Serial.printf("Wake pin %s GPIO%u invalid\n", label, pin);
    return false;
  }
  if (rtc_gpio_init(gpio) != ESP_OK ||
      rtc_gpio_pulldown_dis(gpio) != ESP_OK ||
      rtc_gpio_pullup_en(gpio) != ESP_OK) {
    Serial.printf("RTC pull-up setup failed for %s GPIO%u\n", label, pin);
    return false;
  }

  const esp_sleep_ext1_wakeup_mode_t mode =
      currentHigh ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH;
  const esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << pin, mode);
  if (err != ESP_OK) {
    Serial.printf("Failed to arm %s GPIO%u: %s\n", label, pin, esp_err_to_name(err));
    return false;
  }

  Serial.printf("Wake %s GPIO%u: %s -> wake on %s\n",
                label,
                pin,
                currentHigh ? "HIGH" : "LOW",
                currentHigh ? "LOW" : "HIGH");
  return true;
}

LevelState loadState(esp_sleep_wakeup_cause_t cause) {
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED || rtcMagic != RTC_MAGIC ||
      rtcStateRaw > static_cast<uint8_t>(LevelState::WAIT_HIGH)) {
    rtcMagic = RTC_MAGIC;
    rtcStateRaw = static_cast<uint8_t>(LevelState::NORMAL);
    rtcNormalSleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
    rtcLastWaterTemperatureC = 20.0f;
    rtcNormalSleepValid = false;
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
        plan.nextState = LevelState::NORMAL;
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFloats = true;
        plan.sleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
        plan.watchLow = true;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.reason = "IMPOSSIBLE LOW=CLOSED HIGH=OPEN -> publish fault state";
      } else if (snapshot.lowClosed) {
        plan.nextState = LevelState::LOW_PENDING;
        plan.sleepSeconds = SKIMMERSENSE_LOW_CONFIRM_SECONDS;
        plan.watchLow = true;
        plan.reason = "LOW closed -> continuous confirmation; wake if LOW reopens";
      } else {
        plan.nextState = LevelState::NORMAL;
        plan.sleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
        plan.watchLow = true;
        plan.watchMax = true;
        plan.reason = "normal monitoring: LOW + MAX17048 wake armed";
        plan.useZigbee = coldBoot || timerWake;
        plan.reportTemperature = plan.useZigbee;
        plan.reportFloats = plan.useZigbee;
        if (maxWake) {
          plan.useZigbee = false;  // Battery report path is unsafe in this ZBOSS version.
          plan.reportTemperature = false;
          plan.reportFloats = false;
          plan.reason = "MAX17048 alert acknowledged; battery report intentionally skipped";
        }
      }
      break;

    case LevelState::LOW_PENDING:
      if (!snapshot.lowClosed) {
        plan.nextState = LevelState::NORMAL;
        plan.sleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
        plan.watchLow = true;
        plan.watchMax = true;
        plan.reason = "LOW reopened during confirmation -> rejected as wave/bather motion";
      } else if (!timerWake) {
        plan.nextState = LevelState::LOW_PENDING;
        plan.sleepSeconds = SKIMMERSENSE_LOW_CONFIRM_SECONDS;
        plan.watchLow = true;
        plan.reason = "LOW confirmation interrupted -> restart full confirmation window";
      } else if (!snapshot.highClosed) {
        plan.nextState = LevelState::NORMAL;
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFloats = true;
        plan.sleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
        plan.watchLow = true;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.reason = "IMPOSSIBLE LOW=CLOSED HIGH=OPEN after continuous confirmation -> publish fault state";
      } else {
        plan.nextState = LevelState::WAIT_HIGH;
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFloats = true;
        plan.sleepSeconds = SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.reason = "LOW continuously confirmed -> publish ON/ON, then ignore LOW and watch HIGH";
      }
      break;

    case LevelState::WAIT_HIGH:
      if (!snapshot.highClosed) {
        plan.nextState = LevelState::NORMAL;
        plan.useZigbee = true;
        plan.reportTemperature = true;
        plan.reportFloats = true;
        plan.sleepSeconds = SKIMMERSENSE_NORMAL_TIMER_SECONDS;
        plan.watchLow = true;
        plan.watchMax = true;
        plan.reason = "HIGH opened -> publish final float states and return NORMAL";
      } else {
        plan.nextState = LevelState::WAIT_HIGH;
        plan.sleepSeconds = SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS;
        plan.watchHigh = true;
        plan.watchMax = true;
        plan.reason = "waiting for HIGH to open; LOW transitions intentionally ignored";
        if (timerWake) {
          plan.useZigbee = true;
          plan.reportTemperature = true;
          plan.reportFloats = false;
        }
        if (maxWake) {
          plan.useZigbee = false;
          plan.reportTemperature = false;
          plan.reportFloats = false;
          plan.reason = "MAX17048 alert acknowledged while WAIT_HIGH; continue watching HIGH";
        }
      }
      break;
  }

  // Always announce a cold production boot. When LOW still needs the
  // anti-wave confirmation, publish temperature only: reporting raw floats
  // here could look like a validated refill request to Home Assistant.
  if (coldBoot && !plan.useZigbee) {
    plan.useZigbee = true;
    plan.reportTemperature = true;
    plan.reportFloats = false;
    plan.reason =
        "LOW closed -> confirmation; cold-boot Zigbee heartbeat (temperature only)";
  }

  // Adaptive periodic NORMAL refresh is used only when the LOW
  // float is OPEN. Fault states and LOW-confirmation logic keep their
  // conservative fixed timers. GPIO wake remains immediate.
  if (plan.nextState == LevelState::NORMAL && !snapshot.lowClosed) {
    plan.sleepSeconds = normalSleepSecondsForTemperature(snapshot);
  }

  return plan;
}

[[noreturn]] void enterPlannedSleep(CyclePlan plan) {
  const LevelState previousState =
    static_cast<LevelState>(rtcStateRaw);

  pinMode(PIN_DS18B20_DATA, INPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);

  // If LOW is CLOSED before sleeping in NORMAL, start/restart confirmation.
  // Exclude WAIT_HIGH -> NORMAL because LOW may legitimately still be CLOSED
  // when HIGH opens at the end of a refill.
  if (plan.nextState == LevelState::NORMAL &&
      previousState != LevelState::WAIT_HIGH &&
      plan.watchLow &&
      digitalRead(PIN_FLOAT_LOW) == LOW &&
      digitalRead(PIN_FLOAT_HIGH) == LOW) {

    Serial.println("LOW CLOSED before sleep -> starting/restarting continuous confirmation.");

    plan.nextState = LevelState::LOW_PENDING;
    plan.sleepSeconds = SKIMMERSENSE_LOW_CONFIRM_SECONDS;
    plan.watchLow = true;
    plan.watchHigh = false;
    plan.watchMax = false;
  }

  // HIGH can open while the Zigbee cycle is still awake. If that happens in
  // WAIT_HIGH, do not arm the opposite edge and wait for the long fallback.
  // Force a one-second timer resample so the next boot publishes the fresh
  // LOW/HIGH state and returns to NORMAL.
  if (plan.nextState == LevelState::WAIT_HIGH && plan.watchHigh &&
      digitalRead(PIN_FLOAT_HIGH) == HIGH) {
    Serial.println("HIGH became OPEN before sleep -> immediate 1 s resample.");
    plan.sleepSeconds = 1;
    plan.watchLow = false;
    plan.watchHigh = false;
    plan.watchMax = false;
  }

  rtcMagic = RTC_MAGIC;
  rtcStateRaw = static_cast<uint8_t>(plan.nextState);

  esp_sleep_disable_ext1_wakeup_io(0);
  bool wakeOk = true;

  if (plan.watchLow) {
    const bool high = digitalRead(PIN_FLOAT_LOW) == HIGH;
    wakeOk &= armOppositeLevelWake(PIN_FLOAT_LOW, high, "LOW-float");
  }
  if (plan.watchHigh) {
    const bool high = digitalRead(PIN_FLOAT_HIGH) == HIGH;
    wakeOk &= armOppositeLevelWake(PIN_FLOAT_HIGH, high, "HIGH-float");
  }
  if (plan.watchMax) {
    const bool high = digitalRead(PIN_MAX17048_INT) == HIGH;
    wakeOk &= armOppositeLevelWake(PIN_MAX17048_INT, high, "MAX17048-ALRT");
  }

  const esp_err_t timerErr = esp_sleep_enable_timer_wakeup(plan.sleepSeconds * 1000000ULL);
  if (timerErr != ESP_OK) {
    Serial.printf("Failed to arm timer wake: %s\n", esp_err_to_name(timerErr));
  }

  Serial.printf("Next state: %s\n", stateName(plan.nextState));
  Serial.printf("Deep sleep: %llu s | GPIO wake %s\n",
                static_cast<unsigned long long>(plan.sleepSeconds),
                wakeOk ? "armed as planned" : "PARTIAL/FAILED");
  Serial.println("Going to deep sleep now.");
  skmCycleLogAppend("Next state: %s", stateName(plan.nextState));
  skmCycleLogAppend("Deep sleep: %llu s / GPIO wake %s",
                    static_cast<unsigned long long>(plan.sleepSeconds),
                    wakeOk ? "armed as planned" : "PARTIAL/FAILED");
  const bool captureRequested = skmCycleCaptureRequested();
  if (captureRequested) {
    skmCycleLogAppend("Persistent fifty-wake capture: wake requested");
  }
  skmCycleLogComplete();
  if (captureRequested) {
    const bool captureSaved = skmPersistCycleLogIfRequested();
    Serial.printf("Persistent wake capture: %s | %u wake(s) remaining\n",
                  captureSaved ? "SAVED" : "FAILED",
                  static_cast<unsigned>(skmCycleCaptureRemaining()));
  }
  Serial.flush();
  delay(20);
  esp_deep_sleep_start();
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  skmSelectRadioAntenna();
  delay(SKIMMERSENSE_SERIAL_STARTUP_MS);
  skmCycleLogBegin();
  skmCycleLogAppend("Firmware: %s / %s", FIRMWARE_VERSION, FIRMWARE_FLAVOR);
  skmCycleLogAppend("RF antenna: %s", skmRadioAntennaName());

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const uint64_t extMask = currentExt1Mask();
  const LevelState state = loadState(cause);

  Serial.println();
  Serial.println("========================================");
  Serial.printf(" SkimmerSense v%s\n", FIRMWARE_VERSION);
  Serial.printf(" %s\n", FIRMWARE_FLAVOR);
  Serial.println(" Battery monitoring: MAX17048 enabled");
  Serial.printf(" RF antenna: %s\n", skmRadioAntennaName());
  Serial.println(" Zigbee Power Configuration disabled (ZBOSS workaround)");
  Serial.println("========================================");
  printWakeReason();
  Serial.printf("RTC state: %s\n", stateName(state));
  skmCycleLogAppend("Wake cause: %s", wakeCauseName(cause));
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    skmCycleLogAppend("EXT1 wake mask: 0x%llX",
                      static_cast<unsigned long long>(extMask));
  }
  skmCycleLogAppend("RTC state: %s", stateName(state));

  SensorSnapshot snapshot = readBaseSensorSnapshot();

  // First decide from floats and wake cause only.
  // Power the DS18B20 only if this cycle actually needs a fresh
  // temperature value/report.
  CyclePlan plan = makePlan(state, snapshot, cause, extMask);
  const bool temperatureReadRequested = plan.reportTemperature;

  if (temperatureReadRequested) {
    readTemperatureIntoSnapshot(snapshot);
    plan = makePlan(state, snapshot, cause, extMask);
  }

  Serial.printf("Float LOW : %s\n",
                contactState(snapshot.lowClosed));
  Serial.printf("Float HIGH: %s\n",
                contactState(snapshot.highClosed));

  if (temperatureReadRequested) {
    if (snapshot.temperatureValid) {
      Serial.printf("Water temperature: %.2f C\n",
                    snapshot.waterTemperatureC);
    } else {
      Serial.println("Water temperature: invalid");
    }
  } else {
    Serial.println("Water temperature: skipped for this wake");
  }

  if (snapshot.batteryValid) {
    Serial.printf(
        "MAX17048: VERSION=0x%04X | %.3f V | raw SOC %.1f %% | Zigbee %u %% | INT was %s\n",
        snapshot.maxVersion,
        snapshot.batteryVoltage,
        snapshot.batterySocRaw,
        snapshot.batteryPercent,
        snapshot.maxIntLow ? "LOW" : "HIGH");
  } else {
    Serial.println("MAX17048: unavailable");
  }

  Serial.printf("Decision: %s\n", plan.reason);
  skmCycleLogAppend("Floats: LOW=%s HIGH=%s",
                    contactState(snapshot.lowClosed),
                    contactState(snapshot.highClosed));
  if (snapshot.temperatureValid) {
    skmCycleLogAppend("Water temperature: %.2f C",
                      snapshot.waterTemperatureC);
  } else if (temperatureReadRequested) {
    skmCycleLogAppend("Water temperature: invalid");
  } else {
    skmCycleLogAppend("Water temperature: skipped for this wake");
  }
  if (snapshot.batteryValid) {
    skmCycleLogAppend("MAX17048: %.3f V / raw SOC %.1f %% / rounded %u %% / INT %s",
                      snapshot.batteryVoltage,
                      snapshot.batterySocRaw,
                      snapshot.batteryPercent,
                      snapshot.maxIntLow ? "LOW" : "HIGH");
  } else {
    skmCycleLogAppend("MAX17048: unavailable");
  }
  skmCycleLogAppend("Decision: %s", plan.reason);

#ifdef SKIMMERSENSE_PRODUCTION_BUILD
  if (plan.nextState == LevelState::NORMAL && !snapshot.lowClosed) {
    if (snapshot.temperatureValid) {
      Serial.printf("Adaptive NORMAL timer: %llu s for %.2f C\n",
                    static_cast<unsigned long long>(plan.sleepSeconds),
                    snapshot.waterTemperatureC);
    } else if (rtcNormalSleepValid) {
      Serial.printf(
          "Adaptive NORMAL timer: %llu s (cached from %.2f C)\n",
          static_cast<unsigned long long>(plan.sleepSeconds),
          rtcLastWaterTemperatureC);
    } else {
      Serial.printf(
          "Adaptive NORMAL timer: %llu s (no valid temperature -> fallback)\n",
          static_cast<unsigned long long>(plan.sleepSeconds));
    }
  }
#endif


  if (plan.useZigbee) {
    Serial.println("Preloading Zigbee attributes BEFORE Zigbee.begin()...");
    if (!configureZigbeeEndpoints(snapshot)) {
      Serial.println("Preload/configuration FAILED; sleeping without Zigbee.");
      scheduleZigbeeRecovery(plan, "endpoint configuration failure");
      plan.useZigbee = false;
      plan.reportTemperature = false;
      plan.reportFloats = false;
    } else if (!startZigbee()) {
      scheduleZigbeeRecovery(plan, "startup/reconnection failure");
      plan.useZigbee = false;
      plan.reportTemperature = false;
      plan.reportFloats = false;
    } else {
      Serial.printf("Connected; idling %lu ms without runtime attribute writes...\n",
                    static_cast<unsigned long>(SKIMMERSENSE_ZIGBEE_IDLE_MS));
      delay(SKIMMERSENSE_ZIGBEE_IDLE_MS);

      // Read the parent entry already learned by the stack. This is a local
      // diagnostic lookup and does not transmit an extra Zigbee frame.
      logZigbeeParentReception("before reports");

      bool reportsOk = true;
      if (plan.reportTemperature && snapshot.temperatureValid) {
        reportsOk &= sendSafeReport(
            ZB_EP_TEMPERATURE,
            ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
            "temperature");
        delay(SKIMMERSENSE_BETWEEN_REPORTS_MS);
      }

      if (plan.reportFloats) {
        // LOW first, then HIGH. During refill this lets HA see OFF/ON briefly
        // before OFF/OFF when the high float opens.
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
      }

      if (snapshot.batteryValid) {
        Serial.println("Report battery     : SKIPPED - ZBOSS bug");
      }

      Serial.printf("Post-report confirmation wait: %lu ms...\n",
                    static_cast<unsigned long>(SKIMMERSENSE_POST_REPORT_WAIT_MS));
      delay(SKIMMERSENSE_POST_REPORT_WAIT_MS);

      const bool deliveriesConfirmed = logZigbeeReportConfirmations();
      logZigbeeParentReception("after reports");

      Serial.printf(
          "Zigbee cycle survived. Queueing: %s / delivery confirmations: %s\n",
          reportsOk ? "ALL OK" : "PARTIAL/FAILED",
          deliveriesConfirmed ? "ALL CONFIRMED" : "PARTIAL/MISSING");
      skmCycleLogAppend(
          "Zigbee cycle survived. Queueing: %s / delivery confirmations: %s",
          reportsOk ? "ALL OK" : "PARTIAL/FAILED",
          deliveriesConfirmed ? "ALL CONFIRMED" : "PARTIAL/MISSING");
      if (!reportsOk || !deliveriesConfirmed) {
        scheduleZigbeeRecovery(
            plan,
            reportsOk ? "missing/failed delivery confirmation"
                      : "report queueing failure");
      }
    }
  } else {
    Serial.println("Zigbee cycle intentionally skipped for this state transition.");
    skmCycleLogAppend("Zigbee cycle intentionally skipped");
  }

  enterPlannedSleep(plan);
}

void loop() {
  delay(1000);
}
