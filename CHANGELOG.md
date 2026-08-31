# Changelog

All notable SkimmerSense development milestones are tracked here.

The project has not yet reached a production release. Version labels below refer to development firmware milestones.

## Unreleased / v0.9 preparation

Planned production-oriented cleanup before enabling deep sleep:

- compile-time debug logging control
- bounded Zigbee reconnect/join behavior
- cleaner MAX17048 telemetry abstraction
- Zigbee battery percentage and optional battery voltage
- low-power wake-source preparation for D0, D1 and GPIO4
- sleep-safe DS18B20 shutdown
- production reporting intervals
- measured power-consumption validation

Deep sleep will not be enabled until the real protected 18650 and MAX17048 alert behavior have been validated together.

## v0.8 - MAX17048 post-Zigbee diagnostics

- repeated MAX17048 diagnostics after Zigbee connection so startup information is visible over USB serial
- prints STATUS and CONFIG registers
- prints RCOMP, ALRT and ATHD fields
- reports physical GPIO4 / INT state
- confirms I2C VERSION and battery telemetry registers

Observed on the current prototype:

- MAX17048 address: `0x36`
- VERSION: `0x0012`
- STATUS can be clear while the physical INT line remains low in the USB/no-battery bench condition

This condition is intentionally deferred until a real battery is installed.

## v0.7 - MAX17048 alert acknowledge

- clears startup `STATUS.RI` when present
- acknowledges `CONFIG.ALRT` when set
- reports INT state after acknowledge

## v0.6 - MAX17048 status decoding

- decodes MAX17048 STATUS flags
- identifies reset, SOC and voltage alert causes in serial diagnostics

## v0.5 - MAX17048 INT diagnostics

- adds MAX17048 INT/ALRT input on GPIO4 / MTMS
- reports active-low alert state

## v0.4 - MAX17048 telemetry

- reads VCELL
- reads SOC
- reads CRATE
- reports voltage, state-of-charge estimate and charge-rate estimate over serial

## v0.3 - Zigbee validation timing

- Zigbee End Device operation validated with Zigbee2MQTT
- water-temperature reporting set to a short 60-second bench-test interval
- low/high float endpoints validated

## Earlier bring-up

- XIAO ESP32-C6 PlatformIO environment migrated to `pioarduino` ESP32 platform for Zigbee support
- Zigbee End Device partition/mode configuration added
- DS18B20 switched-power wiring validated
- OneWire library compatibility resolved with OneWireNg
- low and high reed float inputs validated
- BOOT long-press Zigbee factory reset added

## Home Assistant milestones

- temperature and both float entities visible through Zigbee2MQTT
- synthetic `sensor.etat_niveau_piscine` template added
- float truth table validated
- automatic refill dry-run tested through IPX800 V4 relay 6
- Home Assistant timeout and independent IPX800 timeout strategy defined
- dashboard card added for level, water temperature and refill state
