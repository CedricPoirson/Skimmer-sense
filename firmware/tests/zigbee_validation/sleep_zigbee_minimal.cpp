#include <Arduino.h>
#include "Zigbee.h"
#include "esp_sleep.h"

#ifndef SKIMMERSENSE_SLEEP_SECONDS
#define SKIMMERSENSE_SLEEP_SECONDS 1800
#endif

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SkimmerSense Zigbee minimal test");
  Serial.println(" NO attributes");
  Serial.println(" NO reports");
  Serial.println(" ONLY connect + deep sleep");
  Serial.println("========================================");

  Serial.println("Starting Zigbee sleepy End Device...");

  if (!Zigbee.begin()) {
    Serial.println("Zigbee begin failed!");
    return;
  }

  Serial.println("Waiting for Zigbee network");

  while (!Zigbee.connected()) {
    delay(100);
  }

  Serial.println("Zigbee connected!");

  Serial.println("Idle 8000 ms...");
  delay(8000);

  Serial.println("Zigbee cycle OK");

  Serial.printf("Deep sleep: %d seconds\n", SKIMMERSENSE_SLEEP_SECONDS);

  esp_sleep_enable_timer_wakeup(
      (uint64_t)SKIMMERSENSE_SLEEP_SECONDS * 1000000ULL
  );

  Serial.println("Going to deep sleep now.");

  delay(100);

  esp_deep_sleep_start();
}

void loop()
{
}