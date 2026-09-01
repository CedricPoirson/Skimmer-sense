# Power Consumption Notes

SkimmerSense now has a hardware-validated sleepy Zigbee / deep-sleep production-candidate firmware. The remaining power work is measurement and optimization, not basic sleep implementation.

## Current production-candidate behavior

The production PlatformIO environment is:

```text
seeed_xiao_esp32c6_sleep_zigbee_production
```

Current timing profile:

- periodic NORMAL refresh: 1800 s / 30 min
- LOW confirmation: 300 s / 5 min continuously CLOSED
- WAIT_HIGH fallback: 1800 s / 30 min
- bounded Zigbee reconnect wait: 10 s

The firmware:

- sleeps between useful events
- powers the DS18B20 only for a measurement
- places the DS18B20 data pin in high-impedance state before sleep
- wakes on the LOW float while in `NORMAL`
- during `LOW_PENDING`, watches for LOW to reopen and immediately cancels the confirmation if it does
- validates LOW only after the full uninterrupted 5-minute timer window
- ignores LOW transitions while waiting for the HIGH float
- wakes on HIGH opening while in `WAIT_HIGH`
- arms MAX17048 INT/ALRT on GPIO4 where applicable
- performs periodic temperature/float refreshes rather than staying continuously awake

## Anti-wave wake strategy

The RTC-retained state machine is:

```text
NORMAL
  -> LOW_PENDING
  -> WAIT_HIGH
  -> NORMAL
```

This rejects short LOW closures caused by waves or bather motion:

- a LOW closure wakes immediately
- `LOW_PENDING` starts a confirmation timer and arms LOW for the opposite transition
- if LOW reopens before the timer expires, the GPIO wake rejects the event immediately and returns to `NORMAL`
- only a timer wake after a full uninterrupted confirmation window can validate LOW
- after a confirmed low level, LOW is ignored
- HIGH opening remains event-driven and completes the level cycle immediately

The WAIT_HIGH fallback is intentionally 30 minutes. A confirmed low-water request can remain pending for hours until Home Assistant reaches the allowed overnight refill window; there is no need to wake every two minutes while waiting. HIGH opening still wakes immediately, so the longer fallback does not delay refill completion.

Two pre-sleep races are also handled:

- LOW becomes meaningfully CLOSED while the device is still awake -> start/restart `LOW_PENDING` only when both floats are physically consistent with low water
- HIGH opens while Zigbee is still awake in `WAIT_HIGH` -> force a one-second timer-only resample

## Wake sources

| GPIO | Source | Current status |
|---:|---|---|
| GPIO0 / D0 | low-level float | validated hardware wake in `NORMAL` and reopen wake in `LOW_PENDING` |
| GPIO1 / D1 | high-level float | validated hardware wake in `WAIT_HIGH`; also watched in impossible fault state |
| GPIO4 / MTMS | MAX17048 INT/ALRT | armed by firmware; real low-battery ALRT wake still to validate |

The ESP32-C6 EXT1 wake implementation uses state-aware per-pin wake levels so a contact that remains in its current state does not create a wake/sleep loop.

## Zigbee behavior and its energy impact

The current Arduino-ESP32 / ZBOSS stack crashes if the firmware mutates certain Zigbee attributes at runtime and lets automatic reporting process them.

The validated low-power workaround is therefore:

1. wake and read sensors
2. preload temperature and float attributes before `Zigbee.begin()`
3. reconnect with a bounded wait
4. send explicit zero-initialized reports for temperature and/or float states
5. keep Zigbee Power Configuration setup disabled
6. skip explicit battery reporting
7. wait briefly for delivery
8. return to deep sleep

During isolation, explicit `BatteryPercentageRemaining` reporting reproducibly asserted in the current ZBOSS version. `ZigbeeTempSensor::setPowerSource(...)` also caused a crash in this firmware configuration. MAX17048 voltage/SOC monitoring therefore remains local to the firmware/serial diagnostics for now.

## DS18B20 optimization

Current sequence:

1. D2 HIGH powers the DS18B20
2. wait briefly for sensor startup
3. perform a 10-bit conversion
4. read the result
5. put D3 / DATA in high-impedance mode
6. drive D2 LOW before sleep

10-bit resolution is sufficient for pool-water monitoring and reduces awake time compared with 12-bit conversion.

## MAX17048 considerations

The MAX17048 remains connected while the ESP32 sleeps.

Validated so far:

- I2C address `0x36`
- VERSION `0x0012`
- VCELL and SOC reads
- raw SOC diagnostics
- local SOC clamp to 0-100% for diagnostics/future Zigbee use
- INT/ALRT wiring to GPIO4 / MTMS
- inactive INT observed HIGH
- alert acknowledge logic

Still to validate with the final protected 18650:

- VCELL against a multimeter
- SOC plausibility through a real discharge cycle
- a real low-battery threshold / ALRT wake
- long-duration behavior on battery only

## Battery target

Initial battery target: protected 1S 18650 Li-ion, approximately 3500 mAh.

A USB power bank connected to USB-C is **not** representative of the final battery setup: many power banks switch off when the ESP32 enters deep sleep because the load current becomes too small.

The intended final supply is the 1S Li-ion battery path on the XIAO, not a conventional auto-off USB power bank.

## Measurements still required

No final autonomy figure should be published until the assembled device is measured.

Measure current and duration for at least:

1. deep-sleep state
2. periodic 30-minute wake and report
3. DS18B20 conversion
4. normal Zigbee reconnect/report cycle
5. LOW event wake followed by `LOW_PENDING`
6. LOW reopen wake during the 5-minute confirmation window
7. confirmed LOW report and `WAIT_HIGH` entry
8. 30-minute WAIT_HIGH fallback wake
9. HIGH event wake and final report
10. MAX17048 alert wake
11. failed Zigbee reconnect attempt
12. battery-only and USB-charging cases separately

Also remove avoidable hardware loads, especially any permanently-on LED on the MAX17048 breakout, before final sleep-current measurement.

## Measurement model

Calculate average current from the measured duty cycle:

```text
Iavg = sum(I_state x time_state) / total_time
```

Then estimate ideal battery life:

```text
hours ~= usable_capacity_mAh / Iavg_mA
```

The practical field estimate should include margin for:

- cell self-discharge
- outdoor temperature
- battery aging
- Zigbee retransmissions
- coordinator/network outages
- regulator and charger leakage
- breakout LEDs and pull-ups

The target is a conservative autonomy estimate measured on the complete SkimmerSense hardware, not a theoretical figure derived only from component datasheets.
