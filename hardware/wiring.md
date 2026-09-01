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
| MTMS pad | GPIO4 | MAX17048 INT/ALRT | active-low alert / deep-sleep wake input |
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

### Deep-sleep wake use

The production-candidate firmware uses state-aware EXT1 wake behavior:

- in `NORMAL`, GPIO0 / LOW is watched for the meaningful OPEN -> CLOSED transition
- in `LOW_PENDING`, LOW is currently CLOSED and GPIO0 is armed for the opposite CLOSED -> OPEN transition; any reopening before the full 5-minute timer expires cancels confirmation immediately
- only a timer wake after a full uninterrupted confirmation window can validate the low level
- in `WAIT_HIGH`, GPIO1 / HIGH is watched for CLOSED -> OPEN and LOW transitions are deliberately ignored
- in the impossible `LOW=CLOSED / HIGH=OPEN` fault state while `NORMAL`, both float GPIOs are watched so a return to a physically valid state can wake the device promptly

Both LOW and HIGH float wake paths, including LOW reopening during `LOW_PENDING`, have been validated on the real XIAO ESP32-C6.

## DS18B20

The waterproof DS18B20 is powered only during a measurement so its standby consumption is removed during deep sleep.

```text
D2 ----------------------- VCC
 |                          |
 +---- 4.7 kOhm -----------+
                            |
D3 ----------------------- DATA

GND ---------------------- GND
```

Install a 4.7 kOhm pull-up between D2 (switched sensor power) and D3 (1-Wire data), unless the selected sensor adapter already provides an appropriate pull-up.

Current firmware uses 10-bit conversion resolution to reduce measurement time. Before deep sleep, D3 is returned to input/high-impedance and D2 is driven LOW.

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

`INT/ALRT` is active-low and open-drain. It is connected to GPIO4 / MTMS rather than D4. GPIO4 is part of the low-power wake design and is armed as an EXT1 wake input by the production-candidate firmware when applicable.

Important wiring correction discovered during validation:

```text
GPIO4 / MTMS -> INT/ALRT
```

GPIO4 must **not** be connected to `QSTRT`. An earlier bench wiring error connected GPIO4 to `QSTRT`, which produced misleading INT diagnostics. After moving the wire to the real `INT/ALRT` output, the line is HIGH when inactive and the MAX17048 STATUS/CONFIG diagnostics are coherent.

`QSTRT` is not used by the firmware.

The firmware can arm GPIO4 as a wake source and acknowledge MAX17048 alerts, but a real low-battery threshold causing an actual ALRT wake still needs to be validated with the final protected 18650.

The current Zigbee Power Configuration setup is intentionally disabled because `setPowerSource()` caused a reproducible ZBOSS crash in this firmware configuration. MAX17048 monitoring therefore remains local/serial until that stack issue is resolved.

## Battery / charging

The XIAO ESP32-C6 board used for this project includes a single-cell Li-ion charging path. The intended battery is a protected 3.7 V nominal / 4.2 V maximum 18650 of approximately 3500 mAh.

Important points:

- connect the cell only to the battery path, never directly to `VBUS` or `3V3`
- verify JST polarity on the exact MAX17048 breakout before connecting a cell
- the two battery/load JST connectors on the Adafruit-style MAX17048 breakout are electrically common; the gauge measures cell voltage rather than load current
- USB-C on the XIAO is used for charging
- the XIAO `5V` pin is not a battery-output rail when operating only from the 1S battery
- if the MAX17048 breakout includes a permanently-on LED, open/cut its LED jumper before final autonomy measurement

A conventional USB power bank is not a good substitute for the final 1S battery during deep-sleep testing: many power banks shut their 5 V output off when the XIAO current falls below the bank's minimum-load threshold.

## Refill valve boundary

SkimmerSense does **not** directly power or control the refill valve. It only reports sensor information over Zigbee.

The intended fixed-side control path is:

```text
Home Assistant -> IPX800 V4 dry-contact relay -> 24 VAC -> normally-closed irrigation valve
```

The Home Assistant/IPX800 logic has been dry-run tested, but the real solenoid/water path has not yet completed final commissioning.

Automatic refill must never depend on a single software condition. Home Assistant uses state validation and a maximum-open timeout, while the IPX800 has an independent relay timeout as a second safety layer.
