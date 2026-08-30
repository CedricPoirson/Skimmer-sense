# Bill of Materials

Working BOM for the first SkimmerSense prototype.

| Item | Planned part | Notes |
|---|---|---|
| MCU / radio | Seeed Studio XIAO ESP32-C6 | Zigbee-capable, USB-C, Li-ion charging |
| Battery | Protected 18650 Li-ion, 3.7 V, 3500 mAh | Keeppower-class protected cell |
| Battery holder | 1-cell 18650 holder | Removable battery preferred |
| Fuel gauge | MAX17048 module | I2C battery voltage and state-of-charge |
| Water temperature | Waterproof DS18B20 | 3-wire probe |
| Low-level switch | Vertical float switch | Passive reed contact |
| High-level switch | Vertical float switch | Passive reed contact |
| Float connectors | JST-PH 2.0, 2-pin | One per float switch |
| Temperature connector | JST-PH 2.0, 3-pin | If not already supplied with probe |
| DS18B20 pull-up | 4.7 kOhm resistor | Between switched sensor power and data |
| Enclosure | IP67/IP68 plastic enclosure | Final size TBD |
| Cable glands | IP68 glands | Size to match probe / float cables |

## Later / optional

- SHTC3 enclosure humidity sensor
- Reed switch for enclosure/skimmer cover
- Custom PCB after prototype validation
- External 2.4 GHz antenna if required by installation

## Notes

The first prototype should be built and validated on USB power before long-duration battery testing.
