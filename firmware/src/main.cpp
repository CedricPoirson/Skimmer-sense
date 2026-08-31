#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#ifndef ZIGBEE_MODE_ED
#error "SkimmerSense must be built in Zigbee End Device mode"
#endif

#include "Zigbee.h"

// SkimmerSense pinout for Seeed Studio XIAO ESP32-C6
// D0 -> low-level float switch
// D1 -> high-level float switch
// D2 -> switched power for DS18B20
// D3 -> DS18B20 1-Wire data
// D4 -> MAX17048 SDA
// D5 -> MAX17048 SCL
// MTMS / GPIO4 -> MAX17048 INT/ALRT (active LOW)

static constexpr uint8_t PIN_FLOAT_LOW = D0;
static constexpr uint8_t PIN_FLOAT_HIGH = D1;
static constexpr uint8_t PIN_DS18B20_POWER = D2;
static constexpr uint8_t PIN_DS18B20_DATA = D3;
static constexpr uint8_t PIN_I2C_SDA = D4;
static constexpr uint8_t PIN_I2C_SCL = D5;
static constexpr uint8_t PIN_MAX17048_INT = 4;  // GPIO4 / MTMS, NOT D4
static constexpr uint8_t PIN_FACTORY_RESET = BOOT_PIN;

// MAX17048
static constexpr uint8_t MAX17048_I2C_ADDRESS = 0x36;
static constexpr uint8_t MAX17048_REG_VCELL = 0x02;
static constexpr uint8_t MAX17048_REG_SOC = 0x04;
static constexpr uint8_t MAX17048_REG_VERSION = 0x08;
static constexpr uint8_t MAX17048_REG_CRATE = 0x16;
static constexpr uint8_t MAX17048_REG_STATUS = 0x1A;

// STATUS register bits (first byte at address 0x1A).
static constexpr uint8_t MAX17048_STATUS_SC = 0x20;  // SOC changed by 1%
static constexpr uint8_t MAX17048_STATUS_HD = 0x10;  // SOC low
static constexpr uint8_t MAX17048_STATUS_VR = 0x08;  // voltage reset
static constexpr uint8_t MAX17048_STATUS_VL = 0x04;  // voltage low
static constexpr uint8_t MAX17048_STATUS_VH = 0x02;  // voltage high
static constexpr uint8_t MAX17048_STATUS_RI = 0x01;  // reset indicator

// Zigbee endpoints
static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;

// Validation timings. Production battery firmware will use deep sleep.
static constexpr uint32_t TEMP_INTERVAL_MS = 60UL * 1000UL;
static constexpr uint32_t FLOAT_DEBOUNCE_MS = 50;
static constexpr uint32_t MAX17048_CHECK_INTERVAL_MS = 30UL * 1000UL;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
ZigbeeBinary zbLowLevel(ZB_EP_LOW_LEVEL);
ZigbeeBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

bool lastLowRaw = HIGH;
bool lastHighRaw = HIGH;
bool lastMax17048Int = HIGH;
uint32_t lastLowChangeMs = 0;
uint32_t lastHighChangeMs = 0;
uint32_t lastTemperatureMs = 0;
uint32_t lastMax17048CheckMs = 0;

const char *contactState(bool rawState) {
  return rawState == LOW ? "CLOSED" : "OPEN";
}

const char *max17048IntState(bool rawState) {
  return rawState == LOW ? "LOW (ALERT ACTIVE)" : "HIGH (inactive)";
}

bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool readMax17048Register16(uint8_t reg, uint16_t &value) {
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(MAX17048_I2C_ADDRESS, static_cast<uint8_t>(2)) != 2) {
    return false;
  }

  value = (static_cast<uint16_t>(Wire.read()) << 8) |
          static_cast<uint16_t>(Wire.read());
  return true;
}

void printMax17048Status(uint8_t status) {
  Serial.printf("MAX17048 STATUS=0x%02X |", status);

  bool any = false;
  if (status & MAX17048_STATUS_SC) {
    Serial.print(" SC(SOC change)");
    any = true;
  }
  if (status & MAX17048_STATUS_HD) {
    Serial.print(" HD(SOC low)");
    any = true;
  }
  if (status & MAX17048_STATUS_VR) {
    Serial.print(" VR(voltage reset)");
    any = true;
  }
  if (status & MAX17048_STATUS_VL) {
    Serial.print(" VL(voltage low)");
    any = true;
  }
  if (status & MAX17048_STATUS_VH) {
    Serial.print(" VH(voltage high)");
    any = true;
  }
  if (status & MAX17048_STATUS_RI) {
    Serial.print(" RI(reset indicator)");
    any = true;
  }
  if (!any) {
    Serial.print(" no alert cause flags");
  }
  Serial.println();
}

void reportMax17048() {
  Serial.printf("MAX17048 INT: %s\n", max17048IntState(digitalRead(PIN_MAX17048_INT)));

  if (!i2cDevicePresent(MAX17048_I2C_ADDRESS)) {
    Serial.println("MAX17048: no response on I2C address 0x36");
    return;
  }

  uint16_t version = 0;
  if (!readMax17048Register16(MAX17048_REG_VERSION, version)) {
    Serial.println("MAX17048: I2C ACK, but VERSION register read failed");
    return;
  }

  Serial.printf("MAX17048: I2C ACK | VERSION=0x%04X\n", version);

  uint16_t rawStatus = 0;
  if (readMax17048Register16(MAX17048_REG_STATUS, rawStatus)) {
    const uint8_t status = static_cast<uint8_t>(rawStatus >> 8);
    printMax17048Status(status);
  } else {
    Serial.println("MAX17048: STATUS register read failed");
  }

  // Adafruit's reference driver considers the device ready only when
  // (VERSION & 0xFFF0) == 0x0010. Without a battery it may read 0xFFFF.
  if ((version & 0xFFF0) != 0x0010) {
    Serial.println("MAX17048: gauge not ready (battery probably not connected)");
    return;
  }

  uint16_t rawVcell = 0;
  uint16_t rawSoc = 0;
  uint16_t rawCrate = 0;

  if (!readMax17048Register16(MAX17048_REG_VCELL, rawVcell) ||
      !readMax17048Register16(MAX17048_REG_SOC, rawSoc) ||
      !readMax17048Register16(MAX17048_REG_CRATE, rawCrate)) {
    Serial.println("MAX17048: telemetry register read failed");
    return;
  }

  const float voltage = static_cast<float>(rawVcell) * 78.125f / 1000000.0f;
  const float soc = static_cast<float>(rawSoc) / 256.0f;
  const float chargeRate = static_cast<float>(static_cast<int16_t>(rawCrate)) * 0.208f;

  Serial.printf(
      "MAX17048 | voltage: %.3f V | SOC: %.1f %% | rate: %.2f %%/h\n",
      voltage, soc, chargeRate);
}

float readWaterTemperatureC() {
  // The DS18B20 pull-up resistor is between D2 (V) and D3 (DATA).
  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, HIGH);
  delay(20);

  temperatureSensors.begin();

  if (temperatureSensors.getDeviceCount() == 0) {
    pinMode(PIN_DS18B20_DATA, INPUT);
    digitalWrite(PIN_DS18B20_POWER, LOW);
    return DEVICE_DISCONNECTED_C;
  }

  // 10 bits = 0.25 C resolution and about 188 ms conversion time.
  temperatureSensors.setResolution(10);
  temperatureSensors.requestTemperatures();
  const float temperatureC = temperatureSensors.getTempCByIndex(0);

  // Avoid back-powering the sensor while it is switched off.
  pinMode(PIN_DS18B20_DATA, INPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);

  return temperatureC;
}

void publishLowFloat(bool rawState, bool force = false) {
  static bool initialized = false;
  static bool lastPublishedClosed = false;
  const bool closed = rawState == LOW;

  if (!force && initialized && closed == lastPublishedClosed) {
    return;
  }

  initialized = true;
  lastPublishedClosed = closed;

  zbLowLevel.setBinaryInput(closed);
  if (Zigbee.connected()) {
    zbLowLevel.reportBinaryInput();
  }
}

void publishHighFloat(bool rawState, bool force = false) {
  static bool initialized = false;
  static bool lastPublishedClosed = false;
  const bool closed = rawState == LOW;

  if (!force && initialized && closed == lastPublishedClosed) {
    return;
  }

  initialized = true;
  lastPublishedClosed = closed;

  zbHighLevel.setBinaryInput(closed);
  if (Zigbee.connected()) {
    zbHighLevel.reportBinaryInput();
  }
}

void publishTemperature(float temperatureC) {
  if (temperatureC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20: not detected");
    return;
  }

  Serial.printf("Water temperature: %.2f C\n", temperatureC);
  zbTemperature.setTemperature(temperatureC);

  if (Zigbee.connected()) {
    zbTemperature.reportTemperature();
  }
}

void configureZigbeeEndpoints() {
  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(20.0);
  zbTemperature.setTolerance(1);

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

void startZigbee() {
  Serial.println();
  Serial.println("Starting Zigbee End Device...");
  Serial.println("Open 'Permit join' in Zigbee2MQTT if this is the first pairing.");

  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start. Rebooting in 2 seconds...");
    delay(2000);
    ESP.restart();
  }

  Serial.print("Waiting for Zigbee network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(250);
  }
  Serial.println();
  Serial.println("Zigbee connected!");

  zbTemperature.setReporting(1, 60, 1);

  publishLowFloat(digitalRead(PIN_FLOAT_LOW), true);
  publishHighFloat(digitalRead(PIN_FLOAT_HIGH), true);
  publishTemperature(readWaterTemperatureC());
}

void handleFactoryResetButton() {
  if (digitalRead(PIN_FACTORY_RESET) != LOW) {
    return;
  }

  delay(100);
  const uint32_t pressedAt = millis();

  while (digitalRead(PIN_FACTORY_RESET) == LOW) {
    if (millis() - pressedAt > 3000) {
      Serial.println("Factory-resetting Zigbee network data...");
      delay(500);
      Zigbee.factoryReset();
      return;
    }
    delay(20);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  pinMode(PIN_FACTORY_RESET, INPUT_PULLUP);
  pinMode(PIN_FLOAT_LOW, INPUT_PULLUP);
  pinMode(PIN_FLOAT_HIGH, INPUT_PULLUP);
  // MAX17048 INT/ALRT is open-drain and the breakout provides the pull-up.
  pinMode(PIN_MAX17048_INT, INPUT);

  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  pinMode(PIN_DS18B20_DATA, INPUT);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  lastLowRaw = digitalRead(PIN_FLOAT_LOW);
  lastHighRaw = digitalRead(PIN_FLOAT_HIGH);
  lastMax17048Int = digitalRead(PIN_MAX17048_INT);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SkimmerSense v0.6 - MAX17048 alert decode");
  Serial.println(" XIAO ESP32-C6 / Zigbee End Device");
  Serial.println("========================================");
  Serial.printf("Float LOW : %s\n", contactState(lastLowRaw));
  Serial.printf("Float HIGH: %s\n", contactState(lastHighRaw));
  Serial.printf("MAX17048 INT: %s\n", max17048IntState(lastMax17048Int));

  reportMax17048();

  configureZigbeeEndpoints();
  startZigbee();

  lastTemperatureMs = millis();
  lastMax17048CheckMs = millis();

  Serial.println();
  Serial.println("SkimmerSense is online.");
  Serial.println("Temperature interval: 60 seconds (test mode).");
  Serial.println("MAX17048 telemetry interval: 30 seconds.");
  Serial.println("MAX17048 INT/STATUS diagnostic enabled.");
  Serial.println("Hold BOOT for >3 seconds to factory-reset Zigbee pairing.");
}

void loop() {
  const uint32_t now = millis();

  handleFactoryResetButton();

  const bool lowRaw = digitalRead(PIN_FLOAT_LOW);
  if (lowRaw != lastLowRaw && now - lastLowChangeMs >= FLOAT_DEBOUNCE_MS) {
    lastLowRaw = lowRaw;
    lastLowChangeMs = now;
    Serial.printf("LOW-level float changed: %s\n", contactState(lowRaw));
    publishLowFloat(lowRaw);
  }

  const bool highRaw = digitalRead(PIN_FLOAT_HIGH);
  if (highRaw != lastHighRaw && now - lastHighChangeMs >= FLOAT_DEBOUNCE_MS) {
    lastHighRaw = highRaw;
    lastHighChangeMs = now;
    Serial.printf("HIGH-level float changed: %s\n", contactState(highRaw));
    publishHighFloat(highRaw);
  }

  const bool max17048Int = digitalRead(PIN_MAX17048_INT);
  if (max17048Int != lastMax17048Int) {
    lastMax17048Int = max17048Int;
    Serial.printf("MAX17048 INT changed: %s\n", max17048IntState(max17048Int));
  }

  if (now - lastTemperatureMs >= TEMP_INTERVAL_MS) {
    lastTemperatureMs = now;
    publishTemperature(readWaterTemperatureC());
  }

  if (now - lastMax17048CheckMs >= MAX17048_CHECK_INTERVAL_MS) {
    lastMax17048CheckMs = now;
    reportMax17048();
  }

  delay(10);
}
