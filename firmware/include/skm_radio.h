#pragma once

#include <Arduino.h>

#ifndef SKIMMERSENSE_EXTERNAL_ANTENNA
#define SKIMMERSENSE_EXTERNAL_ANTENNA 0
#endif

static_assert(SKIMMERSENSE_EXTERNAL_ANTENNA == 0 ||
              SKIMMERSENSE_EXTERNAL_ANTENNA == 1,
              "SKIMMERSENSE_EXTERNAL_ANTENNA must be 0 or 1");

// XIAO ESP32-C6 RF switch:
// GPIO3 LOW enables switch control; GPIO14 selects ceramic (LOW) or U.FL (HIGH).
inline void skmSelectRadioAntenna() {
  pinMode(WIFI_ENABLE, OUTPUT);
  digitalWrite(WIFI_ENABLE, LOW);
  delay(100);
  pinMode(WIFI_ANT_CONFIG, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG,
               SKIMMERSENSE_EXTERNAL_ANTENNA ? HIGH : LOW);
}

inline const char *skmRadioAntennaName() {
  return SKIMMERSENSE_EXTERNAL_ANTENNA
             ? "external U.FL"
             : "onboard ceramic";
}
