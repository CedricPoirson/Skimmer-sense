#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "soc/soc_caps.h"

#ifndef ZIGBEE_MODE_ED
#error "SkimmerSense must be built in Zigbee End Device mode"
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP_MODE_PER_PIN
#error "SkimmerSense deep-sleep build requires EXT1 per-pin wake levels"
#endif

#ifndef SKIMMERSENSE_SLEEP_SECONDS
#define SKIMMERSENSE_SLEEP_SECONDS 60ULL
#endif

#ifndef SKIMMERSENSE_ZIGBEE_WAIT_MS
#define SKIMMERSENSE_ZIGBEE_WAIT_MS 10000UL
#endif

#ifndef SKIMMERSENSE_REPORT_SETTLE_MS
#define SKIMMERSENSE_REPORT_SETTLE_MS 1500UL
#endif

#include "Zigbee.h"

static constexpr char FIRMWARE_VERSION[] = "0.9-deepsleep-test";

// Seeed Studio XIAO ESP32-C6 pinout.
static constexpr uint8_t PIN_FLOAT_LOW = D0;       // GPIO0, reed to GND
static constexpr uint8_t PIN_FLOAT_HIGH = D1;      // GPIO1, reed to GND
static constexpr uint8_t PIN_DS18B20_POWER = D2;
static constexpr uint8_t PIN_DS18B20_DATA = D3;
static constexpr uint8_t PIN_I2C_SDA = D4;
static constexpr uint8_t PIN_I2C_SCL = D5;
static constexpr uint8_t PIN_MAX17048_INT = 4;     // GPIO4 / MTMS -> ALRT/INT
static constexpr uint8_t PIN_FACTORY_RESET = BOOT_PIN;

// MAX17048 registers.
static constexpr uint8_t MAX17048_I2C_ADDRESS = 0x36;
static constexpr uint8_t MAX17048_REG_VCELL = 0x02;
static constexpr uint8_t MAX17048_REG_SOC = 0x04;
static constexpr uint8_t MAX17048_REG_VERSION = 0x08;
static constexpr uint8_t MAX17048_REG_CONFIG = 0x0C;
static constexpr uint8_t MAX17048_REG_STATUS = 0x1A;
static constexpr uint16_t MAX17048_CONFIG_ALRT = 0x0020;
static constexpr uint8_t MAX17048_STATUS_RI = 0x01;

// Keep the existing endpoint layout unchanged.
static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
ZigbeeBinary zbLowLevel(ZB_EP_LOW_LEVEL);
ZigbeeBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

struct Max17048Telemetry {
  bool valid = false;
  uint16_t version = 0;
  float voltage = 0.0f;
  float soc = 0.0f;
};

const char *contactState(bool rawState) {
  return rawState == LOW ? "CLOSED" : "OPEN";
}

const char *wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1 GPIO";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "cold boot/reset";
    default: return "other";
  }
}

void releaseRtcWakePins() {
  // EXT1 can leave RTC pad configuration active across deep-sleep reset.
  // Return the three wake pins to normal GPIO control before Arduino pinMode().
  rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_FLOAT_LOW));
  rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_FLOAT_HIGH));
  rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_MAX17048_INT));
}

void printWakeReason() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause: %s\n", wakeCauseName(cause));

  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    const uint64_t mask = esp_sleep_get_ext1_wakeup_status();
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

bool readMax17048(Max17048Telemetry &t) {
  t = Max17048Telemetry{};

  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  if (Wire.endTransmission() != 0) return false;

  uint16_t rawVcell = 0;
  uint16_t rawSoc = 0;
  if (!readRegister16(MAX17048_REG_VERSION, t.version) ||
      (t.version & 0xFFF0) != 0x0010 ||
      !readRegister16(MAX17048_REG_VCELL, rawVcell) ||
      !readRegister16(MAX17048_REG_SOC, rawSoc)) {
    return false;
  }

  t.voltage = static_cast<float>(rawVcell) * 78.125f / 1000000.0f;
  t.soc = static_cast<float>(rawSoc) / 256.0f;
  t.valid = true;
  return true;
}

uint8_t batteryPercentage(float soc) {
  if (soc <= 0.0f) return 0;
  if (soc >= 100.0f) return 100;
  return static_cast<uint8_t>(soc + 0.5f);
}

uint8_t batteryVoltageZcl(float voltage) {
  if (voltage <= 0.0f) return 0;
  const float units = voltage * 10.0f;
  if (units >= 254.0f) return 254;
  return static_cast<uint8_t>(units + 0.5f);
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
    writeRegister16(
        MAX17048_REG_STATUS,
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
  const float temperatureC = temperatureSensors.getTempCByIndex(0);

  // Remove all possible parasitic DS18B20 current before sleeping.
  pinMode(PIN_DS18B20_DATA, INPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  return temperatureC;
}

void configureZigbeeEndpoints() {
  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(20.0);
  zbTemperature.setTolerance(1);

  Max17048Telemetry initialBattery;
  uint8_t initialPercentage = 100;
  uint8_t initialVoltage = 40;
  if (readMax17048(initialBattery)) {
    initialPercentage = batteryPercentage(initialBattery.soc);
    initialVoltage = batteryVoltageZcl(initialBattery.voltage);
  }
  zbTemperature.setPowerSource(
      ZB_POWER_SOURCE_BATTERY, initialPercentage, initialVoltage);

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
  Serial.println("Starting Zigbee sleepy End Device...");

  // Follow Espressif's sleepy-device pattern: bounded begin timeout and a
  // 10-second end-device keepalive while awake.
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 10000;
  Zigbee.setTimeout(10000);

  if (!Zigbee.begin(&zigbeeConfig, false)) {
    Serial.println("Zigbee begin failed; sleeping instead of reboot-looping.");
    return false;
  }

  zbTemperature.setReporting(1, 3600, 0.25);

  Serial.print("Waiting for Zigbee network");
  const uint32_t startedAt = millis();
  while (!Zigbee.connected() &&
         millis() - startedAt < SKIMMERSENSE_ZIGBEE_WAIT_MS) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();

  if (!Zigbee.connected()) {
    Serial.println("Zigbee reconnect timeout; will retry after next wake.");
    return false;
  }

  Serial.println("Zigbee connected!");
  return true;
}

void publishSnapshot() {
  // Deep sleep resets the MCU, so always publish the complete current state.
  const bool lowRaw = digitalRead(PIN_FLOAT_LOW);
  const bool highRaw = digitalRead(PIN_FLOAT_HIGH);

  zbLowLevel.setBinaryInput(lowRaw == LOW);
  zbHighLevel.setBinaryInput(highRaw == LOW);

  const float temperatureC = readWaterTemperatureC();
  if (temperatureC != DEVICE_DISCONNECTED_C) {
    zbTemperature.setTemperature(temperatureC);
  }

  Max17048Telemetry battery;
  if (readMax17048(battery)) {
    const uint8_t percentage = batteryPercentage(battery.soc);
    const uint8_t voltage = batteryVoltageZcl(battery.voltage);
    zbTemperature.setBatteryPercentage(percentage);
    zbTemperature.setBatteryVoltage(voltage);
    Serial.printf(
        "Battery attributes: %u %% | %.3f V | raw SOC %.1f %%\n",
        percentage, battery.voltage, battery.soc);
  }

  Serial.printf("Float LOW : %s\n", contactState(lowRaw));
  Serial.printf("Float HIGH: %s\n", contactState(highRaw));
  if (temperatureC != DEVICE_DISCONNECTED_C) {
    Serial.printf("Water temperature: %.2f C\n", temperatureC);
  } else {
    Serial.println("DS18B20: not detected");
  }

  if (Zigbee.connected()) {
    zbLowLevel.reportBinaryInput();
    zbHighLevel.reportBinaryInput();
    if (temperatureC != DEVICE_DISCONNECTED_C) {
      zbTemperature.reportTemperature();
    }
    // Do not call reportBatteryPercentage(): this remains intentionally
    // disabled because it triggered the ZBOSS assertion on Arduino-ESP32 3.3.x.
  }
}

bool armOppositeLevelWake(uint8_t pin, bool currentHigh, const char *label) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  if (!esp_sleep_is_valid_wakeup_gpio(gpio)) {
    Serial.printf("Wake pin %s GPIO%u is invalid\n", label, pin);
    return false;
  }

  // Float switches and MAX17048 ALRT are active-to-GND, so keep a pull-up
  // during deep sleep. On ESP32-C6 GPIO0..7 are RTC wake-capable.
  if (rtc_gpio_init(gpio) != ESP_OK ||
      rtc_gpio_pulldown_dis(gpio) != ESP_OK ||
      rtc_gpio_pullup_en(gpio) != ESP_OK) {
    Serial.printf("RTC pull-up setup failed for %s GPIO%u\n", label, pin);
    return false;
  }

  // EXT1 is level-sensitive. Arm each C6 pin for the *opposite* of its current
  // level, so a held switch cannot cause an immediate wake-sleep loop.
  const esp_sleep_ext1_wakeup_mode_t mode =
      currentHigh ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH;

  const esp_err_t err =
      esp_sleep_enable_ext1_wakeup_io(1ULL << pin, mode);
  if (err != ESP_OK) {
    Serial.printf("Failed to arm %s GPIO%u: %s\n",
                  label, pin, esp_err_to_name(err));
    return false;
  }

  Serial.printf("Wake %s GPIO%u: %s -> wake on %s\n",
                label,
                pin,
                currentHigh ? "HIGH" : "LOW",
                currentHigh ? "LOW" : "HIGH");
  return true;
}

void prepareAndEnterDeepSleep() {
  // Give Zigbee reports time to leave the radio queue before powering down.
  delay(SKIMMERSENSE_REPORT_SETTLE_MS);

  // Re-read after the settle delay in case a float moved while reports were sent.
  const bool lowRaw = digitalRead(PIN_FLOAT_LOW);
  const bool highRaw = digitalRead(PIN_FLOAT_HIGH);
  const bool intRaw = digitalRead(PIN_MAX17048_INT);

  esp_sleep_disable_ext1_wakeup_io(0);

  bool wakePinsOk = true;
  wakePinsOk &= armOppositeLevelWake(PIN_FLOAT_LOW, lowRaw == HIGH, "LOW-float");
  wakePinsOk &= armOppositeLevelWake(PIN_FLOAT_HIGH, highRaw == HIGH, "HIGH-float");
  wakePinsOk &= armOppositeLevelWake(PIN_MAX17048_INT, intRaw == HIGH, "MAX17048-ALRT");

  const esp_err_t timerErr = esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(SKIMMERSENSE_SLEEP_SECONDS) * 1000000ULL);
  if (timerErr != ESP_OK) {
    Serial.printf("Failed to arm timer wake: %s\n", esp_err_to_name(timerErr));
  }

  // DS18B20 must be fully unpowered/high-Z before deep sleep.
  pinMode(PIN_DS18B20_DATA, INPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);

  Serial.printf("Deep sleep: %llu s timer | GPIO wake %s\n",
                static_cast<unsigned long long>(SKIMMERSENSE_SLEEP_SECONDS),
                wakePinsOk ? "armed" : "PARTIAL/FAILED");
  Serial.println("Going to deep sleep now.");
  Serial.flush();
  delay(20);
  esp_deep_sleep_start();
}

bool factoryResetRequestedAtBoot() {
  if (digitalRead(PIN_FACTORY_RESET) != LOW) return false;

  Serial.println("BOOT held: keep holding for 3 seconds to factory-reset Zigbee.");
  const uint32_t startedAt = millis();
  while (digitalRead(PIN_FACTORY_RESET) == LOW &&
         millis() - startedAt < 3100) {
    delay(25);
  }
  return millis() - startedAt >= 3000;
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  releaseRtcWakePins();

  pinMode(PIN_FACTORY_RESET, INPUT_PULLUP);
  pinMode(PIN_FLOAT_LOW, INPUT_PULLUP);
  pinMode(PIN_FLOAT_HIGH, INPUT_PULLUP);
  pinMode(PIN_MAX17048_INT, INPUT_PULLUP);
  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  pinMode(PIN_DS18B20_DATA, INPUT);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  Serial.println();
  Serial.println("========================================");
  Serial.printf(" SkimmerSense v%s\n", FIRMWARE_VERSION);
  Serial.println(" XIAO ESP32-C6 / Zigbee sleepy End Device");
  Serial.println("========================================");
  printWakeReason();

  const bool resetRequested = factoryResetRequestedAtBoot();

  acknowledgeMax17048Alert();
  configureZigbeeEndpoints();
  const bool connected = startZigbee();

  if (resetRequested) {
    Serial.println("Factory-resetting Zigbee network data...");
    delay(250);
    Zigbee.factoryReset();
    return;
  }

  if (connected) {
    publishSnapshot();
  }

  prepareAndEnterDeepSleep();
}

void loop() {
  // setup() always enters deep sleep; loop() should never be reached.
  delay(1000);
}
