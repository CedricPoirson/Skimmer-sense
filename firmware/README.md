# Firmware

SkimmerSense targets the Seeed Studio XIAO ESP32-C6.

## Current status

Phase 1 hardware bring-up is ready:

- USB serial output at 115200 baud
- DS18B20 water-temperature reading
- GPIO-powered DS18B20 for future low-power operation
- low-level float switch input
- high-level float switch input
- simple debounce and live state reporting
- MAX17048 I2C presence check at address `0x36`

The battery, real MAX17048 voltage/SOC readings, Zigbee and deep-sleep will be added after the basic sensors are validated.

> With the Adafruit-style MAX17048 breakout, the MAX17048 itself is powered from the battery by default. Therefore `MAX17048: no response` is expected until a battery is connected, even though VIN is connected to the XIAO 3.3 V rail.

## PlatformIO

The project uses the official PlatformIO board ID `seeed_xiao_esp32c6`.

From the `firmware` directory:

```bash
pio run
pio run -t upload
pio device monitor
```

The serial monitor runs at **115200 baud**.

## Prototype wiring

| XIAO ESP32-C6 | GPIO | Function |
|---|---:|---|
| D0 | GPIO0 | Low-level float switch -> GND |
| D1 | GPIO1 | High-level float switch -> GND |
| D2 | GPIO2 | DS18B20 switched VCC (`V`) |
| D3 | GPIO21 | DS18B20 1-Wire DATA (`S`) |
| D4 | GPIO22 | MAX17048 SDA |
| D5 | GPIO23 | MAX17048 SCL |
| 3V3 | - | MAX17048 VIN |
| GND | - | Common ground |

The float inputs use the ESP32 internal pull-ups, therefore:

- contact open = `HIGH`
- contact closed = `LOW`

For the DS18B20, install a **4.7 kOhm pull-up resistor between D2 (sensor V/power) and D3 (sensor S/data)** unless the specific adapter being used already contains that resistor. This arrangement allows the firmware to remove power from the temperature sensor between measurements later.

## First USB test

The battery is **not required** for this stage.

1. Leave BAT+ / BAT- and the MAX17048 battery connectors empty.
2. Connect the XIAO to the computer over a USB-C data cable.
3. Compile and upload the firmware.
4. Open the serial monitor at 115200 baud.
5. Confirm that both float switches show `OPEN` or `CLOSED` as expected.
6. Move the LOW float by hand and check for a `LOW-level float changed` message.
7. Move the HIGH float by hand and check for a `HIGH-level float changed` message.
8. Confirm a plausible DS18B20 temperature appears every 10 seconds.
9. Until the battery arrives, `MAX17048: no response on I2C address 0x36` is normal.

## Development plan

1. **USB serial bring-up** - ready
2. **DS18B20 temperature reading** - ready for hardware test
3. **Low/high float switch handling** - ready for hardware test
4. **MAX17048 I2C wiring check** - ready; real data requires battery
5. MAX17048 battery voltage and state-of-charge
6. Zigbee device definition and Home Assistant pairing
7. Deep-sleep timer wake-up
8. Wake-up on float switch state changes
9. Power optimization and long-duration battery testing

## Planned Zigbee attributes

- Water temperature
- Low-level state
- High-level state
- Battery percentage
- Battery voltage (if exposed cleanly)

## Target timing

- Periodic temperature/battery report: every 30-60 minutes
- Level changes: transmitted immediately after wake-up

During initial USB testing, temperature is deliberately sampled every 10 seconds to make troubleshooting faster.
