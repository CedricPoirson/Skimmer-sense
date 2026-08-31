# Power Consumption Notes

SkimmerSense is intended to become a battery-powered sleepy Zigbee end device. The current firmware is still a validation build and deliberately stays awake so sensor and Zigbee behavior can be observed over USB serial.

## Current development behavior

The prototype currently:

- remains awake continuously
- samples water temperature frequently for validation
- reads MAX17048 diagnostics frequently
- reports float changes immediately
- keeps serial diagnostics enabled

This behavior is intentionally unsuitable for final battery life.

## Production strategy

The production firmware should minimize both ESP32-C6 awake time and unnecessary radio traffic:

- keep the ESP32-C6 asleep whenever possible
- use a sleepy Zigbee End Device design
- wake periodically for temperature and battery reporting
- wake immediately on meaningful float-state changes
- evaluate MAX17048 INT/ALRT on GPIO4 as a low-battery wake source
- power the DS18B20 only during measurement
- put the DS18B20 data GPIO in high-impedance state before sleeping
- avoid periodic reports when the value has not changed enough to be useful
- avoid indefinite Zigbee join/reconnect loops while running from battery
- cut or disable any permanently-on breakout LEDs

## Intended wake sources

Current pin allocation leaves the important event inputs on GPIOs suitable for low-power wake evaluation:

| GPIO | Source |
|---:|---|
| GPIO0 / D0 | low-level float |
| GPIO1 / D1 | high-level float |
| GPIO4 / MTMS | MAX17048 INT/ALRT |

The exact wake implementation must account for level-triggered wake behavior. A float that remains in the active level must not cause an immediate wake/sleep loop.

A state-aware wake mask or equivalent logic should be used so the next wake is configured for the opposite transition expected from each current float state.

## Periodic reporting

The final interval should be chosen from measured energy use and practical pool-monitoring needs rather than fixed in advance.

Reasonable initial candidates for field testing are:

- water temperature: every 30-60 minutes
- battery SOC: every 1-6 hours, or piggybacked on a temperature wake
- float changes: event-driven as quickly as practical

The current 60-second temperature and 30-second MAX17048 debug intervals are for bench testing only.

## DS18B20 optimization

Current wiring already supports switched sensor power:

1. D2 HIGH powers the DS18B20
2. wait briefly for sensor startup
3. perform a 10-bit conversion
4. read the result
5. put the data pin in high-impedance mode
6. drive D2 LOW before sleep

10-bit resolution is sufficient for pool-water monitoring and reduces conversion time compared with 12-bit operation.

## MAX17048 considerations

The MAX17048 itself is intended to remain connected to the cell while the ESP32 sleeps.

Before optimizing firmware around its alert signal, validate with the real protected 18650 installed:

- VCELL plausibility
- SOC plausibility
- CRATE behavior
- STATUS flags
- CONFIG.ALRT behavior
- physical INT/ALRT voltage and GPIO reading

USB-only readings with no battery installed are not valid for autonomy estimates.

## Battery target

Initial battery: protected 18650 Li-ion, approximately 3500 mAh.

Battery-life estimates should be based on measured current in all important states:

1. deep/sleep state
2. periodic wake without Zigbee retry
3. DS18B20 measurement
4. normal Zigbee report
5. float-event wake and report
6. MAX17048 alert wake
7. Zigbee reconnect/rejoin attempt
8. USB charging / battery-only operation as separate cases

## Measurement model

Once the firmware can sleep reliably, record current and duration for each state and calculate average current from the measured duty cycle:

```text
Iavg = sum(I_state x time_state) / total_time
```

Then estimate ideal battery life:

```text
hours ~= usable_capacity_mAh / Iavg_mA
```

The practical field estimate should then include margin for:

- cell self-discharge
- cold/hot outdoor temperatures
- battery aging
- Zigbee retransmissions
- coordinator/network outages
- regulator and charger leakage
- breakout LEDs or pull-ups

The goal is not a theoretical multi-year number from datasheets; it is a conservative autonomy estimate measured on the assembled SkimmerSense hardware.
