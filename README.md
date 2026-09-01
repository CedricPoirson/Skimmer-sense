# SkimmerSense

Battery-powered Zigbee pool-skimmer monitor for Home Assistant, built around a Seeed Studio XIAO ESP32-C6.

SkimmerSense measures pool-water temperature, monitors two mechanical water-level floats and reads battery telemetry locally through a MAX17048 fuel gauge. Home Assistant uses the two level inputs to supervise a separate refill circuit with multiple safety layers.

## Current status

The `firmware-v0.9` branch is a hardware-validated production candidate.

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

Production-candidate timing profile:

- normal periodic refresh: **30 min**
- LOW-level confirmation: **5 min continuously CLOSED**
- WAIT_HIGH fallback check: **30 min**

GPIO events remain immediate; the 30-minute timers do not delay LOW detection or HIGH-level completion.

Still to validate before merging v0.9 to `main`:

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

## Pin functions used by v0.9

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

The production environment uses:

```text
SKIMMERSENSE_NORMAL_TIMER_SECONDS     = 1800
SKIMMERSENSE_LOW_CONFIRM_SECONDS      = 300
SKIMMERSENSE_WAIT_HIGH_TIMER_SECONDS  = 1800
```

The shorter anti-wave environment remains available for bench testing.

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
- [x] production timing profile: 30 min / 5 min / 30 min
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
