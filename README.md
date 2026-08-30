# SkimmerSense

Battery-powered Zigbee pool skimmer monitor for Home Assistant.

SkimmerSense is a low-power pool monitoring node designed around the Seeed Studio XIAO ESP32-C6. It monitors pool water temperature, high/low water level and battery state, and is intended to integrate with Home Assistant over Zigbee.

## Planned features

- ESP32-C6 based
- Zigbee integration with Home Assistant
- Pool water temperature via waterproof DS18B20
- Low water level detection
- High water level detection
- Battery voltage and state-of-charge via MAX17048
- Rechargeable protected 18650 Li-ion battery
- USB-C battery charging through the XIAO ESP32-C6
- Deep-sleep operation for long battery life
- Wake-up on float switch state change
- Automatic pool refill control through Home Assistant
- Safety timeout and abnormal-level detection

## Hardware

Core components currently planned:

- Seeed Studio XIAO ESP32-C6
- Protected 18650 Li-ion battery, 3.7 V / 3500 mAh
- MAX17048 fuel gauge
- Waterproof DS18B20 temperature probe
- Two vertical float switches
- JST-PH connectors
- IP67/IP68 enclosure

See [`hardware/bom.md`](hardware/bom.md) for the working bill of materials and [`hardware/wiring.md`](hardware/wiring.md) for the current pinout.

## Repository layout

```text
Skimmer-sense/
├── README.md
├── firmware/
├── hardware/
│   ├── bom.md
│   └── wiring.md
├── home-assistant/
└── docs/
```

## Project status

Early hardware design / prototype stage. Parts are being sourced and the firmware will be developed incrementally once the first hardware arrives.

## License

Not selected yet. A permissive open-source license such as MIT may be added once the first working release is ready.
