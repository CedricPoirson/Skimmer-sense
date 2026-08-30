# Home Assistant Integration

SkimmerSense is intended to appear in Home Assistant as a Zigbee sensor device.

## Planned entities

- Water temperature
- Low water level
- High water level
- Battery percentage
- Battery voltage (optional)

## Planned refill logic

Home Assistant will control the pool refill valve separately from the battery-powered skimmer node.

Expected safeguards:

- Start refill only after a confirmed low-level condition
- Stop refill immediately when the high-level condition is reached
- Maximum valve-open timeout
- Alert if the expected level transition does not occur
- Detect physically inconsistent float states
- Optional notifications for low battery or sensor communication loss

Automation YAML will be added after the Zigbee entity names are known from the first working prototype.
