# Firmware

The SkimmerSense firmware will target the Seeed Studio XIAO ESP32-C6.

## Development plan

1. USB serial bring-up
2. DS18B20 temperature reading
3. Low/high float switch input handling
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

## Initial timing target

- Periodic temperature/battery report: every 30-60 minutes
- Level changes: transmitted immediately after wake-up

Implementation details will be added once the first XIAO ESP32-C6 hardware is available for validation.
