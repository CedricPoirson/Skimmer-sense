# SkimmerSense

> **A battery-powered Zigbee sensor that turns a pool skimmer into a safe, observable automatic-refill system for Home Assistant.**

## Why SkimmerSense?

Pool level slowly drops through evaporation, splashing and filter maintenance. Refilling by hand is easy to forget, while opening a valve automatically from a single level switch creates a different risk: waves, swimmers, a stuck sensor or a lost radio message can all produce the wrong decision.

SkimmerSense separates **measurement** from **water control**. A small battery-powered sensor installed in the skimmer measures water temperature and watches two mechanical floats. It sends those states over Zigbee to Home Assistant, which supervises a fixed IPX800 relay and a normally-closed 24 VAC valve.

The result is a local, cloud-free refill system with physical hysteresis, wave rejection, immediate high-level detection and several independent ways to stop the water.

## What it does

| Capability | Benefit |
|---|---|
| Two vertical float switches | Separate refill-start and refill-stop thresholds |
| Five-minute LOW confirmation | Rejects waves, splashing and temporary swimmer movement |
| Immediate GPIO wake | Reacts to meaningful level changes without continuous polling |
| Waterproof DS18B20 | Adds pool-water temperature to Home Assistant |
| Zigbee sleepy End Device | Integrates locally with Zigbee2MQTT while preserving battery life |
| Temperature-adaptive deep sleep | Reports more often when useful and sleeps longer in mild conditions |
| Critical completion redundancy | Sends the final LOW/HIGH state three times when the high level is reached |
| MAX17048 fuel gauge | Measures battery voltage and state of charge locally |
| SERVICE jumper | Enables Wi-Fi diagnostics, retained logs and browser-based OTA updates |
| Layered safety | Home Assistant timeout, independent IPX800 timeout and normally-closed valve |

## Typical refill cycle

1. Water drops below the LOW float.
2. SkimmerSense waits for LOW to remain continuously active for five minutes.
3. A transient caused by a wave or swimmer is rejected immediately if LOW reopens.
4. Once confirmed, Home Assistant may start filling during the configured overnight window.
5. SkimmerSense ignores LOW while filling and watches the HIGH float.
6. When HIGH opens, the ESP32-C6 wakes immediately and publishes the final state three times.
7. Home Assistant closes the valve; independent timeouts remain available if any component fails.

> [!IMPORTANT]
> SkimmerSense never powers the valve directly. The battery device only measures and reports. Water control stays on the fixed Home Assistant/IPX800 side, with a normally-closed valve and an independent maximum-open timeout.

## Project at a glance

- **Sensor:** Seeed Studio XIAO ESP32-C6 with external 2.4 GHz antenna
- **Network:** Zigbee channel 20 through Zigbee2MQTT
- **Measurements:** LOW float, HIGH float, water temperature and local battery telemetry
- **Automation:** Home Assistant controlling an IPX800 V4 dry-contact relay
- **Power:** protected 1S 18650 battery with deep sleep
- **Maintenance:** D6/GPIO16 jumper, Wi-Fi portal, downloadable diagnostics and OTA
- **Current firmware:** v0.9.1 production candidate

## Current status

Firmware **v0.9.1** is a hardware-validated production candidate. Active development, SERVICE-mode diagnostics and Wi-Fi OTA are available on [`feature/service-mode-ota`](https://github.com/CedricPoirson/Skimmer-sense/tree/feature/service-mode-ota).

Validated on the current XIAO ESP32-C6 prototype:

- Zigbee2MQTT join/reconnect as a Zigbee End Device
- existing Zigbee pairing survives normal firmware flashing
- waterproof DS18B20 temperature measurement and reporting
- two vertical reed float switches and their raw Zigbee states
- MAX17048 detected at I2C address `0x36`
- MAX17048 VERSION `0x0012` on the current board
- MAX17048 `INT/ALRT` wiring on XIAO GPIO4, inactive HIGH
- deep sleep with timer wake
- GPIO wake from the float switches
- RTC-retained anti-wave state machine
- continuous LOW confirmation that is cancelled immediately if LOW reopens
- impossible `LOW=CLOSED / HIGH=OPEN` fault handling while in `NORMAL`
- repeated temperature/float Zigbee report cycles without ZBOSS assertion or Guru Meditation
- Home Assistant receives temperature and both float states
- Home Assistant level-state synthesis and dry-contact IPX800 V4 refill-control logic
- independent Home Assistant and IPX800 maximum-open safety timers
- temperature-adaptive periodic wake in `NORMAL`
- immediate LOW GPIO wake remains functional while a long adaptive timer is armed
- production `WAIT_HIGH` 30-minute fallback remains independent from the adaptive `NORMAL` timer
- HIGH GPIO wake returns immediately from `WAIT_HIGH` to `NORMAL` and resumes the correct adaptive interval
- refill completion sends three redundant LOW/HIGH report sequences before sleep

### Production-candidate timing profile

The periodic refresh in `NORMAL` is now selected from the measured water temperature:

| Water temperature | Periodic `NORMAL` wake |
|---|---:|
| >= 28 C | 30 min |
| 24 to < 28 C | 1 h |
| 18 to < 24 C | 2 h |
| 12 to < 18 C | 4 h |
| 5 to < 12 C | 6 h |
| 3 to < 5 C | 2 h |
| < 3 C | 30 min |

If the DS18B20 reading is invalid, production falls back to `SKIMMERSENSE_NORMAL_TIMER_SECONDS` (currently 30 min).

The other state-machine timings remain fixed:

- LOW-level confirmation: **5 min continuously CLOSED**
- WAIT_HIGH fallback check: **30 min**

GPIO events remain immediate. A multi-hour `NORMAL` timer does **not** delay LOW detection, and the 30-minute WAIT_HIGH fallback does **not** delay HIGH-level completion.

The adaptive profile has been exercised on the real prototype at 24.50 C: production selected 3600 s / 1 h, LOW still woke immediately, the full 5-minute confirmation entered `WAIT_HIGH`, the 30-minute fallback behaved correctly, and HIGH opening returned immediately to `NORMAL` with the 3600 s interval restored.

Because the production profile can sleep for up to 6 hours, the Zigbee End Device aging timeout is explicitly set to `ESP_ZB_ED_AGING_TIMEOUT_2048MIN` (about 34 hours), rather than relying on the shorter framework default.

Still to validate before tagging the first production-ready release:

- final protected 1S 18650 in battery-only operation
- real MAX17048 low-battery `INT/ALRT` wake
- final deep-sleep current and battery-life estimate
- several days of unattended stability
- complete physical refill chain once the 24 VAC solenoid valve is installed
- enclosure / long-duration field test

## Architecture

```text
             +----------------------------+
             |        SkimmerSense        |
             |                            |
DS18B20 ---->| XIAO ESP32-C6              |
LOW float -->|                            |
HIGH float ->| Zigbee End Device          |
MAX17048 --->|                            |
             +-------------+--------------+
                           |
                         Zigbee
                           |
                     Zigbee2MQTT
                           |
                    Home Assistant
                           |
                      IPX800 V4
                    dry-contact relay
                           |
                      24 VAC supply
                           |
                 normally-closed valve
                           |
                       pool refill
```

The battery-powered sensor never drives the water valve directly. Refill control remains on the fixed Home Assistant/IPX800 side.

## Hardware

Core components:

- Seeed Studio XIAO ESP32-C6
- protected 1-cell 18650 Li-ion battery, approximately 3500 mAh
- MAX17048 fuel-gauge breakout
- waterproof DS18B20 temperature probe
- two passive vertical reed float switches
- JST-PH connectors
- weather-resistant enclosure

See [`hardware/bom.md`](hardware/bom.md) for the bill of materials and [`hardware/wiring.md`](hardware/wiring.md) for the pinout.

## Current pin functions

| XIAO pin | GPIO | Function |
|---|---:|---|
| D0 | 0 | LOW float, reed to GND |
| D1 | 1 | HIGH float, reed to GND |
| D2 | 2 | switched DS18B20 power |
| D3 | 21 | DS18B20 data |
| D4 | 22 | MAX17048 SDA |
| D5 | 23 | MAX17048 SCL |
| MTMS pad | 4 | MAX17048 `INT/ALRT` |

The MAX17048 `QSTRT` pin is not used by the firmware and must not be confused with `INT/ALRT`.

## Zigbee endpoints

Current validated firmware exposes:

| Endpoint | Function |
|---:|---|
| 10 | Water temperature |
| 11 | Low-level float raw contact state |
| 12 | High-level float raw contact state |

Manufacturer/model strings:

- Manufacturer: `SkimmerSense`
- Model: `SkimmerSense-v1`

MAX17048 voltage/SOC remain available in serial diagnostics, but Zigbee Power Configuration setup/reporting is disabled on this stack because it triggers a reproducible ZBOSS failure.

The float endpoints intentionally expose the physical contact state. Semantic states such as low water, normal level, refill and float fault are derived in Home Assistant.

## Float logic

Both floats use the same electrical convention:

- float down -> reed contact CLOSED -> Zigbee binary `ON`
- float up -> reed contact OPEN -> Zigbee binary `OFF`

With the low float physically below the high float:

| Low float | High float | Meaning |
|---|---|---|
| ON | ON | water below low threshold: refill request |
| OFF | ON | water between thresholds: hysteresis band |
| OFF | OFF | water above high threshold: refill complete |
| ON | OFF | physically inconsistent: fault |

This gives natural mechanical hysteresis between refill start and stop.

## Anti-wave deep-sleep state machine

The production candidate rejects waves and bather motion by requiring a continuous LOW condition.

```text
NORMAL
  LOW closes
      |
      v
LOW_PENDING
  start 5-minute confirmation
  watch LOW for reopening
      |
      +-- LOW reopens before 5 min -> immediate GPIO wake -> reject transient -> NORMAL
      |
      +-- LOW remains CLOSED continuously for 5 min + HIGH CLOSED
              |
              v
          WAIT_HIGH
          publish ON/ON
          ignore LOW transitions
          watch HIGH
              |
              +-- HIGH opens -> immediate GPIO wake -> publish final states -> NORMAL
```

Important behavior validated on hardware:

- in `NORMAL`, LOW wakes the XIAO immediately when it closes
- during `LOW_PENDING`, LOW is armed for the opposite transition; reopening cancels confirmation immediately
- only a timer wake after a full uninterrupted confirmation window can validate LOW
- in `WAIT_HIGH`, LOW transitions are intentionally ignored
- HIGH opening wakes the XIAO immediately
- the WAIT_HIGH timer is **30 min** only as a fallback/periodic check while a refill request may remain pending for hours
- if HIGH changes while Zigbee is still awake just before sleep, the firmware forces a 1-second resample rather than waiting for the fallback timer
- in `NORMAL`, `LOW=CLOSED / HIGH=OPEN` is published as an impossible/fault state and does not enter `LOW_PENDING`
- the adaptive temperature timer only changes periodic `NORMAL` refreshes; it does not modify `LOW_PENDING` or `WAIT_HIGH`

## Zigbee reporting workaround

The current Arduino-ESP32 / ZBOSS stack has reporting problems isolated during v0.9 development.

### Runtime attribute mutation

Calling Arduino Zigbee setters after the stack is running can enter the ZBOSS automatic-reporting path and crash in `zb_zcl_get_next_reporting_info` / `zb_zcl_report_attr`.

The production candidate therefore:

1. wakes and reads the real sensors first
2. preloads the temperature and float values **before** `Zigbee.begin()`
3. reconnects Zigbee
4. sends explicit, zero-initialized reports for temperature and the float states only
5. avoids runtime Zigbee attribute mutation
6. returns to deep sleep

### Battery reporting limitation

During isolation tests, explicit `Power Configuration / Battery Percentage Remaining` reports reproducibly asserted in `esp_zigbee_zcl_command.c:263`, including when addressed directly to coordinator `0x0000` endpoint 1. `ZigbeeTempSensor::setPowerSource(...)` also caused a ZBOSS crash in this firmware configuration.

Current safe behavior:

- MAX17048 voltage and raw SOC are read normally
- raw SOC remains visible in serial diagnostics
- SOC is clamped locally to 0-100% for diagnostics/future use
- Zigbee Power Configuration setup via `setPowerSource()` is disabled
- explicit battery reporting is disabled

This is an intentional workaround, not a missing call. Battery monitoring remains local through the MAX17048 until a framework version is verified to fix the ZBOSS issue.

### Sleepy End Device aging timeout

The longest adaptive `NORMAL` interval is 6 hours. The production firmware therefore configures:

```cpp
zigbeeConfig.nwk_cfg.zed_cfg.ed_timeout =
    ESP_ZB_ED_AGING_TIMEOUT_2048MIN;
```

This gives roughly 34 hours before child aging, providing substantial margin over the longest planned sleep interval and over isolated missed reconnect/report cycles.

## Refill safety concept

The Home Assistant strategy is deliberately conservative:

- automatic refill is allowed only during the configured time window
- LOW must be continuously confirmed before a refill request is published
- the request may remain pending for hours until the allowed overnight window
- refill continues through the middle hysteresis state
- HIGH ends the refill sequence immediately through GPIO wake
- impossible float state is treated as a fault
- unavailable sensor data while filling closes the valve
- Home Assistant enforces a maximum continuous valve-open duration
- the IPX800 relay has its own independent maximum-on timeout
- the intended irrigation valve is normally closed, so loss of 24 VAC closes the water path

The sensor, Zigbee and Home Assistant/IPX dry-contact logic are validated. The final 24 VAC solenoid valve is not yet physically connected, so the complete hydraulic chain is still pending.

See [`home-assistant/README.md`](home-assistant/README.md) for the Home Assistant implementation notes.

## SERVICE mode

A jumper between **D6/GPIO16 and GND**, followed by RESET, starts a maintenance environment instead of Zigbee production mode. The device stays awake, joins the configured home Wi-Fi when available and also creates a private fallback access point.

From a browser, SERVICE mode provides:

- live sensor and battery diagnostics;
- reset history and retained production-cycle traces;
- an optional 50-wake scenario capture;
- downloadable logs;
- firmware upload to the inactive OTA slot;
- access through `http://skimmersense.local/` or the fallback AP.

Remove the jumper and reboot to return to Zigbee/deep-sleep operation. See [`docs/service-mode.md`](docs/service-mode.md) for setup and recovery instructions.

## Firmware

The PlatformIO project is under [`firmware/`](firmware/).

### Production-candidate build

```bash
cd firmware
pio run -e seeed_xiao_esp32c6_sleep_zigbee_production
pio run -e seeed_xiao_esp32c6_sleep_zigbee_production -t upload
pio device monitor
```

Do **not** erase flash/NVS during routine updates. A normal flash preserves the existing Zigbee pairing; an explicit erase can destroy network state and require pairing again.

The production environment keeps these fixed state-machine values:

```text
SKIMMERSENSE_NORMAL_TIMER_SECONDS     = 1800   # conservative fallback / invalid temperature
SKIMMERSENSE_LOW_CONFIRM_SECONDS      = 300
SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS  = 1800
```

`normalSleepSecondsForTemperature()` overrides the first value only for healthy production `NORMAL` operation when a valid DS18B20 measurement is available.

The shorter anti-wave environment remains available for bench testing and continues to use its fixed short timer rather than the adaptive production schedule.

## Repository layout

```text
Skimmer-sense/
├── README.md
├── CHANGELOG.md
├── firmware/
│   ├── V0.9_PLAN.md
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp
│       └── sleep_main.cpp
├── firmware/tests/
│   └── archived diagnostic stages
├── hardware/
│   ├── bom.md
│   └── wiring.md
├── home-assistant/
│   └── README.md
└── docs/
    └── power-consumption.md
```

## Development milestones

- [x] USB/serial bring-up
- [x] DS18B20 temperature measurement
- [x] dual float-switch validation
- [x] Zigbee join and persistent pairing
- [x] Zigbee temperature + float reporting
- [x] MAX17048 I2C communication
- [x] MAX17048 `INT/ALRT` wiring validation
- [x] Home Assistant level-state synthesis
- [x] Home Assistant/IPX800 dry-contact refill logic validation
- [x] sleepy Zigbee / deep sleep
- [x] timer wake from deep sleep
- [x] GPIO wake on float transitions
- [x] anti-wave `NORMAL / LOW_PENDING / WAIT_HIGH` state machine
- [x] continuous 5-minute LOW confirmation with immediate cancellation on reopen
- [x] impossible float-state handling in `NORMAL`
- [x] WAIT_HIGH production fallback: 30 min
- [x] temperature-adaptive `NORMAL` periodic wake: 30 min to 6 h
- [x] immediate LOW/HIGH event wake validated with adaptive timing enabled
- [x] Zigbee End Device aging timeout extended to 2048 min
- [x] isolate and document the ZBOSS runtime-reporting crash
- [x] isolate and document the Zigbee Power Configuration / battery-report crash
- [ ] protected 18650 battery-only validation
- [ ] GPIO wake on a real MAX17048 low-battery alert
- [ ] real deep-sleep current measurements
- [ ] battery-life estimate
- [ ] physical 24 VAC solenoid-valve validation
- [ ] enclosure and long-duration field test

## Safety

Automatic pool refill can cause property damage if a sensor, relay, network or software component fails. Use a normally-closed valve, a manual shutoff, backflow protection appropriate to the installation, and an independent maximum-open timeout outside Home Assistant.

## License

No license has been selected yet. A permissive open-source license may be added when the first production-ready firmware release is tagged.
