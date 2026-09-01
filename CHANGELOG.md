# Changelog

All notable SkimmerSense development milestones are tracked here.

The project has not yet reached a production release. Version labels below refer to development firmware milestones.

## Unreleased / v0.9 production candidate

The current branch now contains the hardware-validated deep-sleep / anti-wave production candidate.

### Deep sleep and wake behavior

- sleepy Zigbee End Device operation validated across repeated deep-sleep cycles
- RTC-retained level state machine added: `NORMAL -> LOW_PENDING -> WAIT_HIGH -> NORMAL`
- LOW float closure wakes the XIAO immediately from `NORMAL`
- LOW confirmation now requires a full uninterrupted 5-minute CLOSED interval
- while in `LOW_PENDING`, LOW reopening wakes immediately and rejects the event as wave/bather motion
- confirmed low level publishes `ON/ON` and enters `WAIT_HIGH`
- LOW transitions are deliberately ignored while in `WAIT_HIGH`
- HIGH opening wakes immediately, publishes final float states and returns to `NORMAL`
- impossible `LOW=CLOSED / HIGH=OPEN` in `NORMAL` is published as a fault and does not enter `LOW_PENDING`
- impossible-state wake monitors both float inputs so a return to a valid state can wake promptly
- pre-sleep LOW race is handled only when both float readings are physically consistent with low water
- pre-sleep HIGH race handled by a one-second timer-only resample
- MAX17048 INT/ALRT is armed as an EXT1 wake source where applicable; a real low-battery alert wake still needs hardware validation

### Production timing profile

`seeed_xiao_esp32c6_sleep_zigbee_production` currently uses:

- periodic NORMAL refresh: 1800 s / 30 min
- LOW confirmation: 300 s / 5 min continuously CLOSED
- WAIT_HIGH fallback: 1800 s / 30 min
- bounded Zigbee reconnect wait: 10 s

The 30-minute WAIT_HIGH fallback does not delay completion: HIGH opening remains GPIO event-driven and wakes immediately. The longer fallback avoids unnecessary battery use when a confirmed low-water request remains pending for hours before the overnight refill window.

### Zigbee / ZBOSS workaround

A crash in the current Arduino-ESP32 / ZBOSS stack was isolated to automatic reporting after runtime attribute mutation and to Power Configuration setup/reporting in this firmware configuration.

Validated workaround:

- read the real sensor snapshot before `Zigbee.begin()`
- preload temperature and float attributes before the Zigbee stack starts
- avoid runtime Arduino Zigbee attribute mutation
- send zero-initialized explicit reports for temperature and float states only
- keep `ZigbeeTempSensor::setPowerSource(...)` disabled
- do **not** explicitly report `BatteryPercentageRemaining`

An explicit battery report reproducibly asserts in `esp_zigbee_zcl_command.c:263`, including when addressed directly to the coordinator. `setPowerSource(...)` also produced a reproducible ZBOSS crash. MAX17048 voltage/SOC monitoring remains available locally/over serial, but reliable active Zigbee battery reporting is intentionally disabled until the upstream stack issue is resolved.

### Hardware validation completed

- existing Zigbee pairing survives normal flashing
- DS18B20 switched-power measurement works in the sleep build
- LOW/HIGH float wake and report sequence validated end to end
- LOW reopen during `LOW_PENDING` validated as an immediate EXT1 wake and confirmation cancellation
- full uninterrupted confirmation validated before transition to `WAIT_HIGH`
- `LOW=CLOSED / HIGH=OPEN` fault handling validated in `NORMAL`
- MAX17048 I2C address `0x36` and VERSION `0x0012` validated
- MAX17048 INT wiring corrected to GPIO4 / MTMS and confirmed HIGH when inactive
- raw SOC is retained for diagnostics while locally clamped to 0-100% for future use
- GitHub Actions build passes for the current production-candidate branch

### Remaining before merge to main

- validate the intended protected 1S 18650 in battery-only operation
- trigger and validate a real MAX17048 low-battery ALRT wake
- measure final deep-sleep and wake-cycle current
- cut/disable any unnecessary MAX17048 breakout LED before final autonomy measurement
- observe the production profile for several days
- connect the real refill valve and run the first supervised water-fill cycle

## v0.8 - MAX17048 post-Zigbee diagnostics

- repeated MAX17048 diagnostics after Zigbee connection so startup information is visible over USB serial
- prints STATUS and CONFIG registers
- prints RCOMP, ALRT and ATHD fields
- reports physical GPIO4 / INT state
- confirms I2C VERSION and battery telemetry registers

Observed on the current prototype:

- MAX17048 address: `0x36`
- VERSION: `0x0012`

The initial apparently-stuck INT line was traced to GPIO4 being connected to `QSTRT` instead of `INT/ALRT`. After correcting the wiring, INT is HIGH when inactive and the register diagnostics are coherent.

## v0.7 - MAX17048 alert acknowledge

- clears startup `STATUS.RI` when present
- acknowledges `CONFIG.ALRT` when set
- reports INT state after acknowledge

## v0.6 - MAX17048 status decoding

- decodes MAX17048 STATUS flags
- identifies reset, SOC and voltage alert causes in serial diagnostics

## v0.5 - MAX17048 INT diagnostics

- adds MAX17048 INT/ALRT input on GPIO4 / MTMS
- reports active-low alert state

## v0.4 - MAX17048 telemetry

- reads VCELL
- reads SOC
- reads CRATE
- reports voltage, state-of-charge estimate and charge-rate estimate over serial

## v0.3 - Zigbee validation timing

- Zigbee End Device operation validated with Zigbee2MQTT
- water-temperature reporting set to a short 60-second bench-test interval
- low/high float endpoints validated

## Earlier bring-up

- XIAO ESP32-C6 PlatformIO environment migrated to `pioarduino` ESP32 platform for Zigbee support
- Zigbee End Device partition/mode configuration added
- DS18B20 switched-power wiring validated
- OneWire library compatibility resolved with OneWireNg
- low and high reed float inputs validated
- BOOT long-press Zigbee factory reset added

## Home Assistant milestones

- temperature and both float entities visible through Zigbee2MQTT
- synthetic `sensor.etat_niveau_piscine` template added
- float truth table validated
- automatic refill dry-run tested through IPX800 V4 relay 6
- Home Assistant timeout and independent IPX800 timeout strategy defined
- dashboard card added for level, water temperature and refill state
- real solenoid/water commissioning remains pending
