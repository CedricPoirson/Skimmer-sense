#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// SkimmerSense prototype pinout for Seeed Studio XIAO ESP32-C6
// D0 / GPIO0  -> low-level float switch
// D1 / GPIO1  -> high-level float switch
// D2 / GPIO2  -> switched power for DS18B20
// D3 / GPIO21 -> DS18B20 1-Wire data
// D4 / GPIO22 -> MAX17048 SDA
// D5 / GPIO23 -> MAX17048 SCL

static constexpr uint8_t PIN_FLOAT_LOW = D0;
static constexpr uint8_t PIN_FLOAT_HIGH = D1;
static constexpr uint8_t PIN_DS18B20_POWER = D2;
static constexpr uint8_t PIN_DS18B20_DATA = D3;
static constexpr uint8_t PIN_I2C_SDA = D4;
static constexpr uint8_t PIN_I2C_SCL = D5;

static constexpr uint8_t MAX17048_I2C_ADDRESS = 0x36;

static constexpr uint32_t TEMP_INTERVAL_MS = 10000;
static constexpr uint32_t FLOAT_DEBOUNCE_MS = 50;
static constexpr uint32_t MAX17048_CHECK_INTERVAL_MS = 30000;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

bool lastLowRaw = HIGH;
bool lastHighRaw = HIGH;
bool lastMax17048Present = false;
uint32_t lastLowChangeMs = 0;
uint32_t lastHighChangeMs = 0;
uint32_t lastTemperatureMs = 0;
uint32_t lastMax17048CheckMs = 0;

const char *contactState(bool rawState) {
  return rawState == LOW ? "CLOSED" : "OPEN";
}

bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void reportMax17048(bool force = false) {
  const bool present = i2cDevicePresent(MAX17048_I2C_ADDRESS);

  if (!force && present == lastMax17048Present) {
    return;
  }

  lastMax17048Present = present;

  if (present) {
    Serial.println("MAX17048: detected on I2C address 0x36");
  } else {
    Serial.println("MAX17048: no response on I2C address 0x36");
    Serial.println("  This is expected while no battery is connected on an Adafruit-style MAX17048 breakout.");
  }
}

float readWaterTemperatureC() {
  // The DS18B20 pull-up resistor must be connected between
  // PIN_DS18B20_POWER (V) and PIN_DS18B20_DATA (S).
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

void printFloatStates() {
  const bool lowRaw = digitalRead(PIN_FLOAT_LOW);
  const bool highRaw = digitalRead(PIN_FLOAT_HIGH);

  Serial.printf(
      "Float switches | LOW: %s | HIGH: %s\n",
      contactState(lowRaw),
      contactState(highRaw));
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  pinMode(PIN_FLOAT_LOW, INPUT_PULLUP);
  pinMode(PIN_FLOAT_HIGH, INPUT_PULLUP);

  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, LOW);
  pinMode(PIN_DS18B20_DATA, INPUT);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  lastLowRaw = digitalRead(PIN_FLOAT_LOW);
  lastHighRaw = digitalRead(PIN_FLOAT_HIGH);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SkimmerSense - hardware bring-up v0.1");
  Serial.println(" XIAO ESP32-C6 / USB test firmware");
  Serial.println("========================================");
  Serial.println();
  Serial.println("Pinout:");
  Serial.println("  D0 -> LOW float -> GND");
  Serial.println("  D1 -> HIGH float -> GND");
  Serial.println("  D2 -> DS18B20 V (switched 3.3 V)");
  Serial.println("  D3 -> DS18B20 S / DATA");
  Serial.println("  D4 -> MAX17048 SDA");
  Serial.println("  D5 -> MAX17048 SCL");
  Serial.println();

  Serial.println("A closed float contact reads LOW because INPUT_PULLUP is enabled.");
  printFloatStates();

  const float temperatureC = readWaterTemperatureC();
  if (temperatureC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20: not detected");
  } else {
    Serial.printf("Water temperature: %.2f C\n", temperatureC);
  }

  reportMax17048(true);

  lastTemperatureMs = millis();
  lastMax17048CheckMs = millis();

  Serial.println();
  Serial.println("Ready. Move each float switch by hand and watch the serial output.");
  Serial.println("Temperature is sampled every 10 seconds during bring-up.");
}

void loop() {
  const uint32_t now = millis();

  const bool lowRaw = digitalRead(PIN_FLOAT_LOW);
  if (lowRaw != lastLowRaw && now - lastLowChangeMs >= FLOAT_DEBOUNCE_MS) {
    lastLowRaw = lowRaw;
    lastLowChangeMs = now;
    Serial.printf("LOW-level float changed: %s\n", contactState(lowRaw));
  }

  const bool highRaw = digitalRead(PIN_FLOAT_HIGH);
  if (highRaw != lastHighRaw && now - lastHighChangeMs >= FLOAT_DEBOUNCE_MS) {
    lastHighRaw = highRaw;
    lastHighChangeMs = now;
    Serial.printf("HIGH-level float changed: %s\n", contactState(highRaw));
  }

  if (now - lastTemperatureMs >= TEMP_INTERVAL_MS) {
    lastTemperatureMs = now;

    const float temperatureC = readWaterTemperatureC();
    if (temperatureC == DEVICE_DISCONNECTED_C) {
      Serial.println("DS18B20: not detected");
    } else {
      Serial.printf("Water temperature: %.2f C\n", temperatureC);
    }
  }

  if (now - lastMax17048CheckMs >= MAX17048_CHECK_INTERVAL_MS) {
    lastMax17048CheckMs = now;
    reportMax17048(false);
  }

  delay(10);
}
