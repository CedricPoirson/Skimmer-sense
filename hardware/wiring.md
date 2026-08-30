# Wiring and Pinout

Current proposed pinout for the first SkimmerSense prototype.

## XIAO ESP32-C6 pin allocation

| XIAO pin | GPIO | Function | Connection |
|---|---:|---|---|
| D0 | GPIO0 | Low-level float | Float switch to GND |
| D1 | GPIO1 | High-level float | Float switch to GND |
| D2 | GPIO2 | DS18B20 switched power | DS18B20 VCC |
| D3 | GPIO21 | DS18B20 data | DS18B20 DATA |
| D4 / SDA | GPIO22 | I2C SDA | MAX17048 SDA |
| D5 / SCL | GPIO23 | I2C SCL | MAX17048 SCL |
| 3V3 | — | I2C logic power | MAX17048 3.3V |
| GND | — | Common ground | All grounds |
| BAT+ / BAT- | — | Battery path | Through battery/fuel-gauge wiring |

Pins D6-D10 are intentionally left free for future expansion.

## Float switches

Each float switch is a passive reed contact.

```text
D0 ---- low-level float ---- GND
D1 ---- high-level float --- GND
```

Firmware should use internal pull-ups and treat the switches as active-low.

The final mechanical orientation of each float must be validated in water before enabling automatic refill control.

## DS18B20

The DS18B20 is powered only during a measurement to reduce standby consumption.

```text
D2 ----------------------- VCC
 |                          |
 +---- 4.7 kOhm -----------+
                            |
D3 ----------------------- DATA

GND ---------------------- GND
```

The 4.7 kOhm resistor is connected between D2 (switched sensor power) and D3 (1-Wire data).

## MAX17048

```text
XIAO 3V3  ---- MAX17048 3.3V
XIAO GND  ---- MAX17048 GND
XIAO D4   ---- MAX17048 SDA
XIAO D5   ---- MAX17048 SCL
```

The ALERT pin is optional for the first prototype.

## Battery / charging

The XIAO ESP32-C6 includes a 1-cell Li-ion charger. A protected 3.7 V 18650 is planned. USB-C on the XIAO is used for charging.

Exact BAT IN / SYS OUT wiring for the selected MAX17048 breakout must be verified against the breakout board documentation before final assembly.

## Safety note

Automatic refill must never depend on a single software condition. The Home Assistant side should include a maximum valve-open duration and abnormal level-state detection.
