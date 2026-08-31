# Firmware

SkimmerSense targets the Seeed Studio XIAO ESP32-C6 and is built with PlatformIO / Arduino.

## Current status

The development firmware has progressed beyond basic bring-up. The following items are working on the prototype:

- USB serial output at 115200 baud
- waterproof DS18B20 temperature reading
- switched DS18B20 power from GPIO D2
- 10-bit DS18B20 conversion
- low-level float switch input on D0
- high-level float switch input on D1
- float debounce and immediate Zigbee reporting
- Zigbee End Device startup and persistent pairing
- Zigbee temperature endpoint
- two Zigbee binary-input endpoints for the raw float contacts
- MAX17048 I2C communication at address `0x36`
- MAX17048 VERSION / STATUS / CONFIG diagnostics
- MAX17048 VCELL / SOC / CRATE diagnostics
- MAX17048 INT/ALRT input connected to GPIO4 / MTMS
- long-press BOOT Zigbee factory reset

The current firmware is still a **validation build**, not the final battery-optimized build. It remains awake and deliberately reports frequently so hardware behavior can be observed over USB serial.

## PlatformIO

From the repository root:

```bash
cd firmware
pio run
pio run -t upload
pio device monitor
```

The serial monitor runs at **115200 baud**.

The project uses `pioarduino`'s ESP32 platform because the firmware requires the ESP32-C6 Zigbee support available there.

## Current pinout

| XIAO ESP32-C6 | GPIO | Function |
|---|---:|---|
| D0 | GPIO0 | low-level float -> GND |
| D1 | GPIO1 | high-level float -> GND |
| D2 | GPIO2 | DS18B20 switched VCC |
| D3 | GPIO21 | DS18B20 1-Wire DATA |
| D4 | GPIO22 | MAX17048 SDA |
| D5 | GPIO23 | MAX17048 SCL |
| MTMS pad | GPIO4 | MAX17048 INT/ALRT |
| 3V3 | - | MAX17048 VIN |
| GND | - | common ground |

See [`../hardware/wiring.md`](../hardware/wiring.md) for the complete wiring notes.

## Zigbee device

Current model strings:

- manufacturer: `SkimmerSense`
- model: `SkimmerSense-v1`

Current endpoints:

| Endpoint | Class | Function |
|---:|---|---|
| 10 | `ZigbeeTempSensor` | pool-water temperature |
| 11 | `ZigbeeBinary` | low float raw contact |
| 12 | `ZigbeeBinary` | high float raw contact |

Float logic is active-low at the GPIO and exposed semantically as:

- contact CLOSED -> binary ON
- contact OPEN -> binary OFF

## Development timing

The validation firmware currently uses short intervals on purpose:

- water temperature: approximately every 60 seconds
- MAX17048 diagnostics: approximately every 30 seconds
- float changes: immediate while awake

These values are **not** intended for the final battery build.

## MAX17048 validation

The gauge is detected and register access works. The current board reports VERSION `0x0012`.

Battery readings taken with the XIAO powered by USB-C while no real cell is connected must not be interpreted as real SOC data. The charger/battery node can be biased in that condition.

The remaining battery-validation sequence is:

1. install the intended protected 18650
2. test with USB-C connected
3. test again on battery only
4. validate VCELL and SOC plausibility
5. validate CRATE direction and stability
6. validate STATUS / CONFIG behavior
7. validate the active-low INT/ALRT line on GPIO4
8. only then expose battery attributes over Zigbee

## v0.9 production-preparation goals

The next firmware milestone should focus on structure and low-power readiness without prematurely enabling deep sleep:

- separate diagnostic logging from production behavior with a compile-time debug switch
- centralize timing constants and production defaults
- make Zigbee reconnect/join behavior bounded rather than waiting forever
- add clean MAX17048 telemetry functions suitable for Zigbee publication
- add battery percentage and, if useful, battery voltage endpoints/attributes
- prepare wake-source handling for D0, D1 and GPIO4
- ensure DS18B20 DATA is high-impedance and sensor power is off before sleep
- preserve Zigbee pairing information across normal firmware updates

Deep sleep should be enabled only after the real battery/MAX17048 behavior has been validated.

## Low-power target

The production concept is a sleepy Zigbee end device rather than a continuously awake sensor.

Expected behavior:

- sleep most of the time
- wake immediately for a meaningful float change
- wake periodically for temperature and battery reporting
- report only changed/required data
- return to sleep after the Zigbee report has completed

The final report interval will be selected from measured power consumption rather than guessed from datasheets.

## Factory reset

Holding the XIAO BOOT button for more than approximately 3 seconds clears Zigbee network data through `Zigbee.factoryReset()`.

Do not erase the complete ESP32 flash during routine firmware updates because that also destroys the stored Zigbee network information and forces re-pairing.
