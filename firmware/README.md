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

Battery, MAX17048, Zigbee and deep-sleep support will be added after the basic sensors are validated.

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
| D2 | GPIO2 | DS18B20 switched VCC |
| D3 | GPIO21 | DS18B20 1-Wire DATA |
| GND | - | Common ground |

The float inputs use the ESP32 internal pull-ups, therefore:

- contact open = `HIGH`
- contact closed = `LOW`

For the DS18B20, install a **4.7 kOhm pull-up resistor between D2 (sensor power) and D3 (data)**. This arrangement allows the firmware to remove power from the temperature sensor between measurements later.

## Development plan

1. **USB serial bring-up** - ready
2. **DS18B20 temperature reading** - ready for hardware test
3. **Low/high float switch handling** - ready for hardware test
4. MAX17048 battery voltage and state-of-charge
5. Zigbee device definition and Home Assistant pairing
6. Deep-sleep timer wake-up
7. Wake-up on float switch state changes
8. Power optimization and long-duration battery testing

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
