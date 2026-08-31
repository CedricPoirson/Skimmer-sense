# Bill of Materials

Working BOM for the first SkimmerSense prototype.

## Ordered / identified hardware

The AliExpress URLs below are stored without tracking parameters. Seller titles and available variants can change over time, so the item ID is the useful long-term reference.

| Function | Part used / ordered | Qty | AliExpress item | Status / notes |
|---|---|---:|---|---|
| MCU / Zigbee radio | Seeed Studio XIAO ESP32-C6 | 1 | [1005006935181127](https://fr.aliexpress.com/item/1005006935181127.html) | ESP32-C6 variant selected; Zigbee End Device firmware validated on the actual board |
| Fuel gauge | MAX17048 Li-ion fuel-gauge breakout | 1 | [1005010386150458](https://fr.aliexpress.com/item/1005010386150458.html) | Actual board has two JST-PH battery/load ports, `INT/ALRT`, `QSTRT`, I2C address `0x36` and a cuttable power-LED jumper |
| Water temperature | Waterproof stainless-steel DS18B20 probe, PH2.0, 1 m cable | 1 | [1005012490880103](https://fr.aliexpress.com/item/1005012490880103.html) | 3-wire probe; validated at 10-bit resolution |
| Battery holder | Single-cell 18650 holder with wire leads | 1 | [32825161070](https://fr.aliexpress.com/item/32825161070.html) | Removable 1S 18650 holder; this order link was supplied twice, so it is listed once here |

The XIAO listing currently contains several XIAO variants. SkimmerSense specifically uses the **XIAO ESP32-C6**, not the C3/C5 alternatives. The DS18B20 listing is a waterproof 1 m PH2.0 probe. The 18650 holder listing is a single-cell 3.7 V holder. The MAX17048 item is documented from the board actually fitted to the prototype, since the seller listing title is not reliably indexed.

## Other prototype components

| Item | Part | Status / notes |
|---|---|---|
| Battery | Protected 18650 Li-ion, 1S, 3.7 V nominal / 4.2 V max, ~3500 mAh | Final battery-only validation still pending |
| Low-level switch | Vertical passive reed float switch | Validated; float down = CLOSED / Zigbee ON |
| High-level switch | Vertical passive reed float switch | Validated; float down = CLOSED / Zigbee ON |
| Float connectors | JST-PH 2.0, 2-pin | One per float switch |
| DS18B20 pull-up | 4.7 kOhm resistor | Between switched D2 power and D3 data |
| Enclosure | Weather-resistant / IP-rated plastic enclosure | Final installation pending |
| Cable glands | IP-rated glands | Size to match probe and float cables |

## MAX17048 board details

The prototype uses an Adafruit-style MAX17048 breakout layout with:

- I2C address `0x36`
- two electrically common JST-PH battery/load connectors
- `INT/ALRT` wired to XIAO GPIO4 / MTMS
- `QSTRT` not used by the firmware
- green power LED on the front
- `LED` cut jumper on the rear, which can be opened later to remove the LED load before final autonomy measurements

The MAX17048 itself measures cell voltage / state-of-charge; it does **not** measure instantaneous load current.

## Wiring-specific parts

- DS18B20: GND -> GND, VCC -> D2, DATA -> D3
- 4.7 kOhm pull-up between D2 and D3
- low float -> D0 / GPIO0 to GND
- high float -> D1 / GPIO1 to GND
- MAX17048 SDA -> D4 / GPIO22
- MAX17048 SCL -> D5 / GPIO23
- MAX17048 `INT/ALRT` -> GPIO4 / MTMS

See [`wiring.md`](wiring.md) for the complete validated pinout.

## Later / optional

- SHTC3 enclosure humidity sensor
- reed switch for enclosure/skimmer cover
- custom PCB after prototype validation
- external 2.4 GHz antenna if required by installation

## Remaining hardware validation

Before declaring the BOM final:

1. install and validate the intended protected 1S 18650 in battery-only operation
2. test a real MAX17048 low-battery `INT/ALRT` wake
3. cut/disable the MAX17048 breakout LED before the final low-power measurement if autonomy warrants it
4. measure deep-sleep current and complete wake/report energy
5. validate the enclosure and cable glands outdoors
6. validate the complete 24 VAC refill-valve chain under supervision
