#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// SkimmerSense prototype pinout for Seeed Studio XIAO ESP32-C6
// D0 / GPIO0  -> low-level float switch
// D1 / GPIO1  -> high-level float switch
// D2 / GPIO2  -> switched power for DS18B20
// D3 / GPIO21 -> DS18B20 1-Wire data

static constexpr uint8_t PIN_FLOAT_LOW = 0;
static constexpr uint8_t PIN_FLOAT_HIGH = 1;
static constexpr uint8_t PIN_DS18B20_POWER = 2;
static constexpr uint8_t PIN_DS18B20_DATA = 21;

static constexpr uint32_t TEMP_INTERVAL_MS = 10000;
static constexpr uint32_t FLOAT_DEBOUNCE_MS = 50;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature temperatureSensors(&oneWire);

bool lastLowRaw = HIGH;
bool lastHighRaw = HIGH;
uint32_t lastLowChangeMs = 0;
uint32_t lastHighChangeMs = 0;
uint32_t lastTemperatureMs = 0;

const char *contactState(bool rawState) {
  return rawState == LOW ? "CLOSED" : "OPEN";
}

float readWaterTemperatureC() {
  // The DS18B20 pull-up resistor must be connected between
  // PIN_DS18B20_POWER and PIN_DS18B20_DATA.
  pinMode(PIN_DS18B20_POWER, OUTPUT);
  digitalWrite(PIN_DS18B20_POWER, HIGH);
  delay(20);

  temperatureSensors.begin();
  temperatureSensors.setResolution(10); // 0.25 C resolution, ~188 ms conversion
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

  lastLowRaw = digitalRead(PIN_FLOAT_LOW);
  lastHighRaw = digitalRead(PIN_FLOAT_HIGH);

  Serial.println();
  Serial.println("SkimmerSense - hardware bring-up");
  Serial.println("XIAO ESP32-C6 / USB test firmware");
  Serial.println("A closed float contact reads LOW because INPUT_PULLUP is enabled.");
  printFloatStates();

  const float temperatureC = readWaterTemperatureC();
  if (temperatureC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20: not detected");
  } else {
    Serial.printf("Water temperature: %.2f C\n", temperatureC);
  }

  lastTemperatureMs = millis();
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

  delay(10);
}
