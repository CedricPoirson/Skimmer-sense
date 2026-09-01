# Changelog

All notable SkimmerSense development milestones are tracked here.

The project has not yet reached a production release. Version labels below refer to development firmware milestones.

## Unreleased / v0.9 production candidate

The current branch now contains the hardware-validated deep-sleep / anti-wave production candidate with a temperature-adaptive periodic wake profile.

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

### Temperature-adaptive production timing

`seeed_xiao_esp32c6_sleep_zigbee_production` now selects the periodic `NORMAL` refresh from the measured DS18B20 water temperature:

| Water temperature | Periodic `NORMAL` wake |
|---|---:|
| >= 28 C | 1800 s / 30 min |
| 24 to < 28 C | 3600 s / 1 h |
| 18 to < 24 C | 7200 s / 2 h |
| 12 to < 18 C | 14400 s / 4 h |
| 5 to < 12 C | 21600 s / 6 h |
| 3 to < 5 C | 7200 s / 2 h |
| < 3 C | 1800 s / 30 min |

If the temperature reading is invalid, the firmware falls back to the conservative fixed `SKIMMERSENSE_NORMAL_TIMER_SECONDS` value, currently 1800 s / 30 min.

Fixed state-machine timings remain:

- LOW confirmation: 300 s / 5 min continuously CLOSED
- WAIT_HIGH fallback: 1800 s / 30 min
- bounded Zigbee reconnect wait: 10 s

The adaptive schedule applies only to healthy `NORMAL` operation. LOW and HIGH float transitions remain GPIO event-driven, so longer periodic sleeps do not delay level detection or refill completion.

The 30-minute WAIT_HIGH fallback does not delay completion: HIGH opening remains GPIO event-driven and wakes immediately. The fallback avoids unnecessary battery use when a confirmed low-water request remains pending for hours before the overnight refill window.

### Adaptive profile hardware validation

The production build was exercised end to end on the real prototype at 24.50 C:

- `NORMAL` selected 3600 s / 1 h as expected for 24-28 C
- LOW closed during the 1-hour sleep and woke the device immediately
- the fixed 300 s uninterrupted LOW confirmation remained active
- confirmed LOW entered `WAIT_HIGH`
- the fixed 1800 s WAIT_HIGH fallback woke, reported temperature and remained in WAIT_HIGH while HIGH stayed CLOSED
- HIGH opening woke immediately, published final float states and returned to `NORMAL`
- after returning to `NORMAL`, the 3600 s adaptive interval was restored
- temperature, LOW and HIGH reports continued to queue successfully without a ZBOSS crash

### Zigbee sleepy-child aging timeout

Because adaptive `NORMAL` sleep can reach 6 hours, production now explicitly configures:

```cpp
zigbeeConfig.nwk_cfg.zed_cfg.ed_timeout =
    ESP_ZB_ED_AGING_TIMEOUT_2048MIN;
```

This provides about 34 hours of child-aging margin over the longest planned sleep interval.

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
- production and anti-wave PlatformIO environments compile after the adaptive timing change
- GitHub Actions build passes for the current production-candidate branch

### Remaining before merge to main

- validate the intended protected 1S 18650 in battery-only operation
- trigger and validate a real MAX17048 low-battery ALRT wake
- measure final deep-sleep and wake-cycle current
- cut/disable any unnecessary MAX17048 breakout LED before final autonomy measurement
- quantify battery life with the adaptive wake profile
- observe the production profile for several days, including 4-hour and 6-hour sleep intervals
- verify Zigbee2MQTT/Home Assistant availability behavior across the longest sleeps
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
