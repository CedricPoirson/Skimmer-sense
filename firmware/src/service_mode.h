#pragma once

#include <Arduino.h>

// D6 (GPIO16) is unused by SkimmerSense and is exposed on the XIAO header.
// Close a jumper between D6 and GND, then reset/power-cycle the board to enter
// service mode. GPIO16 is not an ESP32-C6 RTC GPIO, so this jumper cannot wake
// the board directly from deep sleep.
static constexpr uint8_t SKIMMERSENSE_SERVICE_PIN = D6;

enum class SkmDiagStage : uint8_t {
  BOOT = 0,
  SENSORS = 1,
  PLAN = 2,
  ZIGBEE_START = 3,
  ZIGBEE_CONNECTED = 4,
  REPORTING = 5,
  SLEEP = 6,
  SERVICE = 7,
};

using SkmServiceStatusProvider = String (*)();

bool skmServiceRequested();

// Call once, very early in setup(). This records non-deep-sleep resets in NVS
// while keeping the normal periodic deep-sleep cycle flash-write-free.
void skmDiagnosticsBoot();

// The following calls only update RTC memory. They do not write flash.
void skmDiagSetStage(SkmDiagStage stage);
void skmDiagSetSensors(bool lowClosed, bool highClosed);
void skmDiagSetPlan(uint8_t state, uint8_t nextState, uint32_t sleepSeconds);
void skmDiagSetZigbeeAttempt();
void skmDiagSetZigbeeConnected(bool connected);
void skmDiagSetReportsResult(bool ok);
void skmDiagSetSleep(uint8_t nextState, uint32_t sleepSeconds);

// Retained production-cycle trace. The buffer lives in RTC no-init memory:
// normal cycles never write it to flash, and a RESET into SERVICE mode can
// display the last cycle. A power loss intentionally clears its validity.
void skmCycleLogBegin();
void skmCycleLogAppend(const char *format, ...);
void skmCycleLogComplete();
bool skmCycleLogAvailable();
bool skmCycleLogIsComplete();
String skmCycleLogSnapshot();

// A capture request is stored in NVS by SERVICE mode. The next 50 production
// wakes are appended to persistent storage unless capture is cancelled.
bool skmRequestNextCycleCapture();
bool skmCancelCycleCapture();
bool skmCycleCaptureRequested();
uint8_t skmCycleCaptureRemaining();
bool skmPersistCycleLogIfRequested();
String skmPersistedCycleLogSnapshot();

const char *skmDiagStageName(SkmDiagStage stage);

// Starts a battery-powered local maintenance access point and never returns.
// Zigbee is intentionally not started in this mode.
[[noreturn]] void skmRunServiceMode(const char *firmwareVersion,
                                    const char *firmwareFlavor,
                                    SkmServiceStatusProvider statusProvider);
