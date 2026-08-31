#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "soc/soc_caps.h"

#if !SOC_PM_SUPPORT_EXT1_WAKEUP_MODE_PER_PIN
#error "SkimmerSense GPIO sleep test requires EXT1 per-pin wake levels"
#endif

#ifndef SKIMMERSENSE_SLEEP_SECONDS
#define SKIMMERSENSE_SLEEP_SECONDS 60ULL
#endif

static constexpr char FIRMWARE_VERSION[] = "0.9-deepsleep-gpio-test";

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
static constexpr uint8_t MAX17048_REG_CONFIG = 0x0C;
static constexpr uint8_t MAX17048_REG_STATUS = 0x1A;
static constexpr uint16_t MAX17048_CONFIG_ALRT = 0x0020;
static constexpr uint8_t MAX17048_STATUS_RI = 0x01;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

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

void acknowledgeMax17048Alert() {
  uint16_t rawStatus = 0;
  uint16_t config = 0;
  if (!readRegister16(MAX17048_REG_STATUS, rawStatus) ||
      !readRegister16(MAX17048_REG_CONFIG, config)) {
    Serial.println("MAX17048 acknowledge: register read failed");
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

void printMax17048Snapshot() {
  uint16_t version = 0;
  uint16_t rawVcell = 0;
  uint16_t rawSoc = 0;

  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  if (Wire.endTransmission() != 0 ||
      !readRegister16(MAX17048_REG_VERSION, version) ||
      !readRegister16(MAX17048_REG_VCELL, rawVcell) ||
      !readRegister16(MAX17048_REG_SOC, rawSoc)) {
    Serial.println("MAX17048: unavailable");
    return;
  }

  const float voltage = static_cast<float>(rawVcell) * 78.125f / 1000000.0f;
  const float soc = static_cast<float>(rawSoc) / 256.0f;
  Serial.printf("MAX17048: VERSION=0x%04X | %.3f V | SOC %.1f %% | INT %s\n",
                version,
                voltage,
                soc,
                digitalRead(PIN_MAX17048_INT) == LOW ? "LOW" : "HIGH");
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

  pinMode(PIN_DS18B20_DATA, INPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  return temperatureC;
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

void setup() {
  Serial.begin(115200);
  delay(1200);

  releaseRtcWakePins();

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
  Serial.println(" GPIO/timer deep-sleep isolation test");
  Serial.println(" Zigbee intentionally NOT started");
  Serial.println("========================================");
  printWakeReason();

  acknowledgeMax17048Alert();

  const bool lowRaw = digitalRead(PIN_FLOAT_LOW);
  const bool highRaw = digitalRead(PIN_FLOAT_HIGH);
  const bool intRaw = digitalRead(PIN_MAX17048_INT);

  Serial.printf("Float LOW : %s\n", contactState(lowRaw));
  Serial.printf("Float HIGH: %s\n", contactState(highRaw));
  printMax17048Snapshot();

  const float temperatureC = readWaterTemperatureC();
  if (temperatureC != DEVICE_DISCONNECTED_C) {
    Serial.printf("Water temperature: %.2f C\n", temperatureC);
  } else {
    Serial.println("DS18B20: not detected");
  }

  // Important: isolate deep-sleep GPIO/timer behaviour from Zigbee reporting.
  // The previous test crashed in the first explicit Zigbee attribute report,
  // before the code reached deep sleep. This build deliberately sends no Zigbee
  // commands so wake-source behaviour can be validated independently.
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

void loop() {
  delay(1000);
}
