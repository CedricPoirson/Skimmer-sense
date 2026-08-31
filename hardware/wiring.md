# Wiring and Pinout

Validated prototype wiring for SkimmerSense based on the Seeed Studio XIAO ESP32-C6.

## XIAO ESP32-C6 pin allocation

| XIAO pin | GPIO | Function | Connection |
|---|---:|---|---|
| D0 | GPIO0 | Low-level float | Float switch to GND |
| D1 | GPIO1 | High-level float | Float switch to GND |
| D2 | GPIO2 | DS18B20 switched power | DS18B20 VCC |
| D3 | GPIO21 | DS18B20 data | DS18B20 DATA |
| D4 / SDA | GPIO22 | I2C SDA | MAX17048 SDA |
| D5 / SCL | GPIO23 | I2C SCL | MAX17048 SCL |
| MTMS pad | GPIO4 | MAX17048 INT/ALRT | active-low alert input |
| 3V3 | — | MAX17048 logic power | MAX17048 VIN |
| GND | — | Common ground | all grounds |
| BAT+ / BAT- | — | 1-cell battery path | battery through MAX17048 breakout |

D6-D10 remain free for future expansion.

## Float switches

Each level switch is a passive reed contact connected between its GPIO and GND:

```text
D0 ---- low-level float ---- GND
D1 ---- high-level float --- GND
```

The firmware uses `INPUT_PULLUP`, therefore:

- contact OPEN -> GPIO HIGH -> Zigbee binary OFF
- contact CLOSED -> GPIO LOW -> Zigbee binary ON

Both floats have been mechanically oriented with the same logic:

- float physically down -> CLOSED / ON
- float physically up -> OPEN / OFF

The low float is mounted below the high float. This produces the following physical truth table:

| Low | High | Physical meaning |
|---|---|---|
| ON | ON | below low threshold |
| OFF | ON | between thresholds |
| OFF | OFF | above high threshold |
| ON | OFF | impossible / fault |

The spacing between the two switches provides the refill hysteresis.

## DS18B20

The waterproof DS18B20 is powered only during a measurement so its standby consumption can later be eliminated during sleep.

```text
D2 ----------------------- VCC
 |                          |
 +---- 4.7 kOhm -----------+
                            |
D3 ----------------------- DATA

GND ---------------------- GND
```

Install a 4.7 kOhm pull-up between D2 (switched sensor power) and D3 (1-Wire data), unless the selected sensor adapter already provides an appropriate pull-up.

Current firmware uses 10-bit conversion resolution to reduce measurement time.

## MAX17048

Validated I2C wiring:

```text
XIAO 3V3       ---- MAX17048 VIN
XIAO GND       ---- MAX17048 GND
XIAO D4/GPIO22 ---- MAX17048 SDA
XIAO D5/GPIO23 ---- MAX17048 SCL
XIAO GPIO4     ---- MAX17048 INT/ALRT
```

The device responds at I2C address `0x36` and the current board reports VERSION `0x0012`.

`INT/ALRT` is active-low and open-drain. It is connected to GPIO4 / MTMS rather than D4. GPIO4 was selected so it can later be evaluated as a low-power wake source.

`QSTRT` is intentionally left disconnected during normal operation.

The final MAX17048 ALERT behavior must be validated again with the real 18650 installed. Readings taken while the XIAO is powered only from USB-C and the battery connector is empty are not considered valid battery telemetry.

## Battery / charging

The XIAO ESP32-C6 board used for this project includes a single-cell Li-ion charging path. The intended battery is a protected 3.7 V nominal / 4.2 V maximum 18650 of approximately 3500 mAh.

Important points:

- connect the cell only to the battery path, never to `VBUS` or `3V3`
- verify JST polarity on the exact MAX17048 breakout before connecting a cell
- the two battery/load JST connectors on the Adafruit-style MAX17048 breakout are electrically common; the gauge measures cell voltage rather than load current
- USB-C on the XIAO is used for charging
- if the breakout includes a permanently-on LED, its jumper should be opened for the final low-power build

## Refill valve boundary

SkimmerSense does **not** directly power or control the refill valve. It only reports sensor information over Zigbee.

The current fixed-side control path is:

```text
Home Assistant -> IPX800 V4 dry-contact relay -> 24 VAC -> normally-closed irrigation valve
```

Automatic refill must never depend on a single software condition. Home Assistant uses state validation and a maximum-open timeout, while the IPX800 has an independent relay timeout as a second safety layer.
