# SkimmerSense

Battery-powered Zigbee pool-skimmer monitor for Home Assistant, built around a Seeed Studio XIAO ESP32-C6.

SkimmerSense measures pool-water temperature, monitors two mechanical water-level floats and is intended to report battery state through a MAX17048 fuel gauge. Home Assistant uses the two level inputs to control a separate refill valve with multiple safety layers.

## Current status

The prototype is now functional over Zigbee:

- XIAO ESP32-C6 joins Zigbee2MQTT as a Zigbee End Device
- waterproof DS18B20 temperature probe validated
- two vertical reed float switches validated
- float changes are reported to Zigbee immediately while the prototype is awake
- MAX17048 detected at I2C address `0x36`
- MAX17048 VERSION register validated (`0x0012` on the current board)
- Home Assistant receives temperature and both float states
- Home Assistant template sensor combines the two float states into a human-readable pool-level state
- automatic refill control has been validated with a dry-contact IPX800 V4 relay
- independent Home Assistant and IPX800 maximum-open timers are used for refill safety

Still in development:

- validation of MAX17048 voltage/SOC/ALERT behavior with the final 18650 installed
- Zigbee battery percentage/voltage exposure
- sleepy-end-device / deep-sleep power optimization
- GPIO wake-up from float changes and battery alert
- real current measurements and battery-life estimation

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

Validated / selected core components:

- Seeed Studio XIAO ESP32-C6
- protected 1-cell 18650 Li-ion battery, approximately 3500 mAh
- MAX17048 fuel-gauge breakout
- waterproof DS18B20 temperature probe
- two passive vertical reed float switches
- JST-PH connectors
- weather-resistant enclosure

See [`hardware/bom.md`](hardware/bom.md) for the bill of materials and [`hardware/wiring.md`](hardware/wiring.md) for the validated pinout.

## Zigbee endpoints

Current firmware exposes:

| Endpoint | Function |
|---:|---|
| 10 | Water temperature |
| 11 | Low-level float raw contact state |
| 12 | High-level float raw contact state |

Manufacturer/model strings:

- Manufacturer: `SkimmerSense`
- Model: `SkimmerSense-v1`

The float endpoints intentionally expose the physical contact state. Semantic states such as `Low water`, `Normal`, `Refilling` and `Float fault` are derived in Home Assistant.

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

## Refill safety concept

The current Home Assistant strategy is deliberately conservative:

- automatic refill is only allowed during a defined quiet time window
- low-level state must remain stable before refill starts
- refill continues through the middle hysteresis state
- high-level state must remain stable before refill stops
- impossible float state closes the valve and generates an alert
- unavailable sensor data while filling closes the valve
- Home Assistant enforces a maximum continuous valve-open duration
- the IPX800 relay also has its own independent hardware-side timeout
- the irrigation valve is normally closed, so loss of 24 VAC closes the water path

See [`home-assistant/README.md`](home-assistant/README.md) for the Home Assistant implementation notes.

## Firmware

The PlatformIO project is under [`firmware/`](firmware/).

```bash
cd firmware
pio run
pio run -t upload
pio device monitor
```

The current development firmware is intentionally verbose and uses short reporting intervals for bench validation. The next production-oriented milestone will focus on sleepy Zigbee behavior, wake sources and battery telemetry.

## Repository layout

```text
Skimmer-sense/
├── README.md
├── CHANGELOG.md
├── firmware/
│   ├── platformio.ini
│   └── src/main.cpp
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
- [x] Home Assistant level-state synthesis
- [x] Home Assistant/IPX800 refill logic validation
- [ ] final 18650 + MAX17048 validation
- [ ] battery attributes over Zigbee
- [ ] sleepy Zigbee / deep sleep
- [ ] GPIO wake on float transitions
- [ ] GPIO wake on MAX17048 alert
- [ ] real power measurements
- [ ] enclosure and long-duration field test

## Safety

Automatic pool refill can cause property damage if a sensor, relay, network or software component fails. Use a normally-closed valve, a manual shutoff, backflow protection appropriate to the installation, and an independent maximum-open timeout outside Home Assistant.

## License

No license has been selected yet. A permissive open-source license may be added when the first production-ready firmware release is tagged.
