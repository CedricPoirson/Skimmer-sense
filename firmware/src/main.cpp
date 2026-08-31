#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#ifndef ZIGBEE_MODE_ED
#error "SkimmerSense must be built in Zigbee End Device mode"
#endif

#ifndef SKIMMERSENSE_DEBUG
#define SKIMMERSENSE_DEBUG 1
#endif

#ifndef SKIMMERSENSE_TEMP_INTERVAL_MS
#define SKIMMERSENSE_TEMP_INTERVAL_MS (60UL * 1000UL)
#endif

#ifndef SKIMMERSENSE_MAX17048_INTERVAL_MS
#define SKIMMERSENSE_MAX17048_INTERVAL_MS (30UL * 1000UL)
#endif

#ifndef SKIMMERSENSE_ZIGBEE_WAIT_MS
#define SKIMMERSENSE_ZIGBEE_WAIT_MS (60UL * 1000UL)
#endif

#include "Zigbee.h"

static constexpr char FIRMWARE_VERSION[] = "0.9-battery-zigbee";

// SkimmerSense pinout for Seeed Studio XIAO ESP32-C6.
static constexpr uint8_t PIN_FLOAT_LOW = D0;
static constexpr uint8_t PIN_FLOAT_HIGH = D1;
static constexpr uint8_t PIN_DS18B20_POWER = D2;
static constexpr uint8_t PIN_DS18B20_DATA = D3;
static constexpr uint8_t PIN_I2C_SDA = D4;
static constexpr uint8_t PIN_I2C_SCL = D5;
static constexpr uint8_t PIN_MAX17048_INT = 4;  // GPIO4 / MTMS -> MAX17048 ALRT/INT
static constexpr uint8_t PIN_FACTORY_RESET = BOOT_PIN;

// MAX17048 registers.
static constexpr uint8_t MAX17048_I2C_ADDRESS = 0x36;
static constexpr uint8_t MAX17048_REG_VCELL = 0x02;
static constexpr uint8_t MAX17048_REG_SOC = 0x04;
static constexpr uint8_t MAX17048_REG_VERSION = 0x08;
static constexpr uint8_t MAX17048_REG_CONFIG = 0x0C;
static constexpr uint8_t MAX17048_REG_CRATE = 0x16;
static constexpr uint8_t MAX17048_REG_STATUS = 0x1A;

// STATUS first-byte bits.
static constexpr uint8_t MAX17048_STATUS_SC = 0x20;
static constexpr uint8_t MAX17048_STATUS_HD = 0x10;
static constexpr uint8_t MAX17048_STATUS_VR = 0x08;
static constexpr uint8_t MAX17048_STATUS_VL = 0x04;
static constexpr uint8_t MAX17048_STATUS_VH = 0x02;
static constexpr uint8_t MAX17048_STATUS_RI = 0x01;

// CONFIG low byte: bit 5 = ALRT, bits 4:0 = ATHD.
static constexpr uint16_t MAX17048_CONFIG_ALRT = 0x0020;

// Zigbee endpoints. Do not change: Home Assistant currently relies on them.
static constexpr uint8_t ZB_EP_TEMPERATURE = 10;
static constexpr uint8_t ZB_EP_LOW_LEVEL = 11;
static constexpr uint8_t ZB_EP_HIGH_LEVEL = 12;

// Bench-validation timings. Deep sleep is deliberately not enabled yet.
static constexpr uint32_t TEMP_INTERVAL_MS = SKIMMERSENSE_TEMP_INTERVAL_MS;
static constexpr uint32_t FLOAT_DEBOUNCE_MS = 50;
static constexpr uint32_t BATTERY_INTERVAL_MS = SKIMMERSENSE_MAX17048_INTERVAL_MS;
static constexpr uint32_t ZIGBEE_CONNECT_WAIT_MS = SKIMMERSENSE_ZIGBEE_WAIT_MS;

struct Max17048Telemetry {
  bool present = false;
  bool versionValid = false;
  bool statusValid = false;
  bool configValid = false;
  bool telemetryValid = false;
  uint16_t version = 0;
  uint16_t rawStatus = 0;
  uint16_t config = 0;
  float voltage = 0.0f;
  float soc = 0.0f;
  float rate = 0.0f;
};

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

// Battery Power Configuration cluster is attached to endpoint 10 so the
// device keeps the same three endpoints already known by Zigbee2MQTT/HA.
ZigbeeTempSensor zbTemperature(ZB_EP_TEMPERATURE);
ZigbeeBinary zbLowLevel(ZB_EP_LOW_LEVEL);
ZigbeeBinary zbHighLevel(ZB_EP_HIGH_LEVEL);

bool stableLowRaw = HIGH;
bool stableHighRaw = HIGH;
bool candidateLowRaw = HIGH;
bool candidateHighRaw = HIGH;
bool lastMax17048Int = HIGH;
bool zigbeeWasConnected = false;

uint8_t lastPublishedBatteryPercentage = 0xFF;
uint8_t lastPublishedBatteryVoltage = 0xFF;

uint32_t lowCandidateSinceMs = 0;
uint32_t highCandidateSinceMs = 0;
uint32_t lastTemperatureMs = 0;
uint32_t lastBatteryMs = 0;

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

bool writeMax17048Register16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(MAX17048_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool readMax17048Telemetry(Max17048Telemetry &t) {
  t = Max17048Telemetry{};

  if (!i2cDevicePresent(MAX17048_I2C_ADDRESS)) {
    return false;
  }
  t.present = true;

  if (!readMax17048Register16(MAX17048_REG_VERSION, t.version)) {
    return false;
  }

  t.versionValid = (t.version & 0xFFF0) == 0x0010;
  t.statusValid = readMax17048Register16(MAX17048_REG_STATUS, t.rawStatus);
  t.configValid = readMax17048Register16(MAX17048_REG_CONFIG, t.config);

  uint16_t rawVcell = 0;
  uint16_t rawSoc = 0;
  uint16_t rawCrate = 0;

  if (!t.versionValid ||
      !readMax17048Register16(MAX17048_REG_VCELL, rawVcell) ||
      !readMax17048Register16(MAX17048_REG_SOC, rawSoc) ||
      !readMax17048Register16(MAX17048_REG_CRATE, rawCrate)) {
    return t.present;
  }

  t.voltage = static_cast<float>(rawVcell) * 78.125f / 1000000.0f;
  t.soc = static_cast<float>(rawSoc) / 256.0f;
  t.rate = static_cast<float>(static_cast<int16_t>(rawCrate)) * 0.208f;
  t.telemetryValid = true;
  return true;
}

uint8_t zigbeeBatteryPercentage(float soc) {
  if (soc <= 0.0f) {
    return 0;
  }
  if (soc >= 100.0f) {
    return 100;
  }
  return static_cast<uint8_t>(soc + 0.5f);
}

uint8_t zigbeeBatteryVoltage(float voltage) {
  // ZCL BatteryVoltage uses units of 100 mV: 4.05 V -> 41.
  if (voltage <= 0.0f) {
    return 0;
  }
  const float units = voltage * 10.0f;
  if (units >= 254.0f) {
    return 254;
  }
  return static_cast<uint8_t>(units + 0.5f);
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
  Max17048Telemetry t;

  Serial.printf("MAX17048 INT: %s\n",
                max17048IntState(digitalRead(PIN_MAX17048_INT)));

  if (!readMax17048Telemetry(t) || !t.present) {
    Serial.println("MAX17048: no response on I2C address 0x36");
    return;
  }

  Serial.printf("MAX17048: I2C ACK | VERSION=0x%04X\n", t.version);

  if (t.statusValid) {
    printMax17048Status(static_cast<uint8_t>(t.rawStatus >> 8));
  } else {
    Serial.println("MAX17048 STATUS: read failed");
  }

  if (t.configValid) {
    const uint8_t rcomp = static_cast<uint8_t>(t.config >> 8);
    const uint8_t configLow = static_cast<uint8_t>(t.config & 0xFF);
    const uint8_t athd = configLow & 0x1F;
    Serial.printf(
        "MAX17048 CONFIG=0x%04X | RCOMP=0x%02X | ALRT=%s | ATHD(raw)=%u\n",
        t.config,
        rcomp,
        (t.config & MAX17048_CONFIG_ALRT) ? "SET" : "clear",
        athd);
  } else {
    Serial.println("MAX17048 CONFIG: read failed");
  }

  if (!t.versionValid) {
    Serial.println("MAX17048: unexpected VERSION value");
    return;
  }

  if (!t.telemetryValid) {
    Serial.println("MAX17048: telemetry register read failed");
    return;
  }

  Serial.printf(
      "MAX17048 | voltage: %.3f V | SOC: %.1f %% | rate: %.2f %%/h\n",
      t.voltage, t.soc, t.rate);
}

void acknowledgeMax17048Alert() {
  uint16_t rawStatus = 0;
  uint16_t config = 0;

  if (!readMax17048Register16(MAX17048_REG_STATUS, rawStatus) ||
      !readMax17048Register16(MAX17048_REG_CONFIG, config)) {
    Serial.println("MAX17048: unable to read STATUS/CONFIG for acknowledge");
    return;
  }

  const uint8_t status = static_cast<uint8_t>(rawStatus >> 8);

  if (status & MAX17048_STATUS_RI) {
    const uint16_t clearedStatus =
        rawStatus & ~(static_cast<uint16_t>(MAX17048_STATUS_RI) << 8);
    if (writeMax17048Register16(MAX17048_REG_STATUS, clearedStatus)) {
      Serial.println("MAX17048: STATUS.RI cleared");
    } else {
      Serial.println("MAX17048: failed to clear STATUS.RI");
    }
  }

  if (config & MAX17048_CONFIG_ALRT) {
    const uint16_t newConfig = config & ~MAX17048_CONFIG_ALRT;
    if (writeMax17048Register16(MAX17048_REG_CONFIG, newConfig)) {
      Serial.println("MAX17048: CONFIG.ALRT acknowledged");
    } else {
      Serial.println("MAX17048: failed to clear CONFIG.ALRT");
    }
  }

  delay(20);
  lastMax17048Int = digitalRead(PIN_MAX17048_INT);
  Serial.printf("MAX17048 INT after acknowledge: %s\n",
                max17048IntState(lastMax17048Int));
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

  // Prevent parasitic current through DATA during future sleep.
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

bool publishBatteryFromMax17048(bool force = false) {
  Max17048Telemetry t;
  if (!readMax17048Telemetry(t) || !t.telemetryValid) {
    if (SKIMMERSENSE_DEBUG) {
      Serial.println("Zigbee battery: MAX17048 telemetry unavailable");
    }
    return false;
  }

  const uint8_t percentage = zigbeeBatteryPercentage(t.soc);
  const uint8_t voltage = zigbeeBatteryVoltage(t.voltage);
  const bool percentageChanged = percentage != lastPublishedBatteryPercentage;
  const bool voltageChanged = voltage != lastPublishedBatteryVoltage;

  if (force || percentageChanged) {
    if (!zbTemperature.setBatteryPercentage(percentage)) {
      Serial.println("Zigbee battery: failed to set percentage attribute");
      return false;
    }
  }

  if (force || voltageChanged) {
    if (!zbTemperature.setBatteryVoltage(voltage)) {
      Serial.println("Zigbee battery: failed to set voltage attribute");
      return false;
    }
  }

  // BatteryPercentageRemaining is reportable. BatteryVoltage is a standard
  // Power Configuration attribute but the Arduino Zigbee helper does not
  // provide a reportBatteryVoltage() method, so it remains readable on demand.
  if (Zigbee.connected() && (force || percentageChanged)) {
    if (!zbTemperature.reportBatteryPercentage()) {
      Serial.println("Zigbee battery: percentage report failed");
      return false;
    }
  }

  lastPublishedBatteryPercentage = percentage;
  lastPublishedBatteryVoltage = voltage;

  if (SKIMMERSENSE_DEBUG || force) {
    Serial.printf(
        "Zigbee battery: %u %% | %.3f V (ZCL voltage=%u x 100mV) | raw SOC %.1f %%\n",
        percentage,
        t.voltage,
        voltage,
        t.soc);
  }

  return true;
}

void configureZigbeeEndpoints() {
  zbTemperature.setManufacturerAndModel("SkimmerSense", "SkimmerSense-v1");
  zbTemperature.setMinMaxValue(-10, 60);
  zbTemperature.setDefaultValue(20.0);
  zbTemperature.setTolerance(1);

  // Add the standard ZCL Power Configuration cluster (0x0001) on endpoint 10.
  // Seed it from the MAX17048 before Zigbee.begin(), because the cluster list
  // must exist when the endpoint is registered with the Zigbee stack.
  Max17048Telemetry initialBattery;
  uint8_t initialPercentage = 100;
  uint8_t initialVoltage = 40;  // 4.0 V fallback for bench discovery only.
  if (readMax17048Telemetry(initialBattery) && initialBattery.telemetryValid) {
    initialPercentage = zigbeeBatteryPercentage(initialBattery.soc);
    initialVoltage = zigbeeBatteryVoltage(initialBattery.voltage);
  }

  if (!zbTemperature.setPowerSource(
          ZB_POWER_SOURCE_BATTERY, initialPercentage, initialVoltage)) {
    Serial.println("Zigbee battery: failed to add Power Configuration cluster");
  } else {
    Serial.printf(
        "Zigbee battery cluster ready: %u %% | %u x 100mV\n",
        initialPercentage,
        initialVoltage);
  }

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

void publishCurrentState() {
  publishLowFloat(stableLowRaw, true);
  publishHighFloat(stableHighRaw, true);
  publishTemperature(readWaterTemperatureC());
  publishBatteryFromMax17048(true);
}

void handleZigbeeConnectionChange() {
  const bool connected = Zigbee.connected();

  if (connected && !zigbeeWasConnected) {
    Serial.println("Zigbee connected!");
    publishCurrentState();
  } else if (!connected && zigbeeWasConnected) {
    Serial.println("Zigbee connection lost; waiting for stack recovery.");
  }

  zigbeeWasConnected = connected;
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

  zbTemperature.setReporting(1, 60, 1);

  Serial.print("Waiting for Zigbee network");
  const uint32_t startedAt = millis();
  while (!Zigbee.connected() && millis() - startedAt < ZIGBEE_CONNECT_WAIT_MS) {
    Serial.print(".");
    delay(250);
  }
  Serial.println();

  if (!Zigbee.connected()) {
    Serial.println("Zigbee network wait timed out; continuing without blocking.");
  }

  handleZigbeeConnectionChange();
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

void serviceFloatDebounce(uint32_t now) {
  const bool lowRaw = digitalRead(PIN_FLOAT_LOW);
  if (lowRaw != candidateLowRaw) {
    candidateLowRaw = lowRaw;
    lowCandidateSinceMs = now;
  } else if (candidateLowRaw != stableLowRaw &&
             now - lowCandidateSinceMs >= FLOAT_DEBOUNCE_MS) {
    stableLowRaw = candidateLowRaw;
    Serial.printf("LOW-level float changed: %s\n", contactState(stableLowRaw));
    publishLowFloat(stableLowRaw);
  }

  const bool highRaw = digitalRead(PIN_FLOAT_HIGH);
  if (highRaw != candidateHighRaw) {
    candidateHighRaw = highRaw;
    highCandidateSinceMs = now;
  } else if (candidateHighRaw != stableHighRaw &&
             now - highCandidateSinceMs >= FLOAT_DEBOUNCE_MS) {
    stableHighRaw = candidateHighRaw;
    Serial.printf("HIGH-level float changed: %s\n", contactState(stableHighRaw));
    publishHighFloat(stableHighRaw);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  pinMode(PIN_FACTORY_RESET, INPUT_PULLUP);
  pinMode(PIN_FLOAT_LOW, INPUT_PULLUP);
  pinMode(PIN_FLOAT_HIGH, INPUT_PULLUP);

  // MAX17048 ALRT/INT is open-drain. GPIO4 now connects to the actual ALRT
  // output (not QSTRT). Internal pull-up is useful for the bench build.
  pinMode(PIN_MAX17048_INT, INPUT_PULLUP);

  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  pinMode(PIN_DS18B20_DATA, INPUT);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  stableLowRaw = digitalRead(PIN_FLOAT_LOW);
  stableHighRaw = digitalRead(PIN_FLOAT_HIGH);
  candidateLowRaw = stableLowRaw;
  candidateHighRaw = stableHighRaw;
  lastMax17048Int = digitalRead(PIN_MAX17048_INT);
  lowCandidateSinceMs = millis();
  highCandidateSinceMs = millis();

  Serial.println();
  Serial.println("========================================");
  Serial.printf(" SkimmerSense v%s\n", FIRMWARE_VERSION);
  Serial.println(" XIAO ESP32-C6 / Zigbee End Device");
  Serial.println("========================================");
  Serial.printf("Float LOW : %s\n", contactState(stableLowRaw));
  Serial.printf("Float HIGH: %s\n", contactState(stableHighRaw));
  Serial.printf("MAX17048 INT: %s\n", max17048IntState(lastMax17048Int));

  if (SKIMMERSENSE_DEBUG) {
    reportMax17048();
    acknowledgeMax17048Alert();
  }

  configureZigbeeEndpoints();
  startZigbee();

  lastTemperatureMs = millis();
  lastBatteryMs = millis();

  Serial.println();
  Serial.println("SkimmerSense is online.");
  Serial.printf("Temperature interval: %lu seconds (bench mode).\n",
                static_cast<unsigned long>(TEMP_INTERVAL_MS / 1000UL));
  Serial.printf("Battery/MAX17048 interval: %lu seconds.\n",
                static_cast<unsigned long>(BATTERY_INTERVAL_MS / 1000UL));
  Serial.println("Zigbee battery percentage: enabled on Power Configuration cluster 0x0001.");
  Serial.println("Deep sleep: disabled until real-battery validation.");
  Serial.println("Hold BOOT for >3 seconds to factory-reset Zigbee pairing.");

  if (SKIMMERSENSE_DEBUG) {
    delay(1000);
    Serial.println();
    Serial.println("--- MAX17048 POST-ZIGBEE DIAGNOSTIC ---");
    reportMax17048();
    acknowledgeMax17048Alert();
    Serial.printf("GPIO4 / MAX17048 INT final: %s\n",
                  max17048IntState(digitalRead(PIN_MAX17048_INT)));
    Serial.println("--- END MAX17048 DIAGNOSTIC ---");
  }
}

void loop() {
  const uint32_t now = millis();

  handleFactoryResetButton();
  handleZigbeeConnectionChange();
  serviceFloatDebounce(now);

  const bool max17048Int = digitalRead(PIN_MAX17048_INT);
  if (max17048Int != lastMax17048Int) {
    lastMax17048Int = max17048Int;
    Serial.printf("MAX17048 INT changed: %s\n",
                  max17048IntState(max17048Int));
  }

  if (now - lastTemperatureMs >= TEMP_INTERVAL_MS) {
    lastTemperatureMs = now;
    publishTemperature(readWaterTemperatureC());
  }

  if (now - lastBatteryMs >= BATTERY_INTERVAL_MS) {
    lastBatteryMs = now;
    publishBatteryFromMax17048(false);
    if (SKIMMERSENSE_DEBUG) {
      reportMax17048();
    }
  }

  delay(10);
}
