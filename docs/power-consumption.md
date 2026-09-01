# Power Consumption Notes

SkimmerSense now has a hardware-validated sleepy Zigbee / deep-sleep production-candidate firmware. The remaining power work is measurement and optimization, not basic sleep implementation.

## Current production-candidate behavior

The production PlatformIO environment is:

```text
seeed_xiao_esp32c6_sleep_zigbee_production
```

### Adaptive `NORMAL` wake profile

Periodic refresh is selected from the measured water temperature:

| Water temperature | Periodic `NORMAL` wake |
|---|---:|
| >= 28 C | 1800 s / 30 min |
| 24 to < 28 C | 3600 s / 1 h |
| 18 to < 24 C | 7200 s / 2 h |
| 12 to < 18 C | 14400 s / 4 h |
| 5 to < 12 C | 21600 s / 6 h |
| 3 to < 5 C | 7200 s / 2 h |
| < 3 C | 1800 s / 30 min |

If a fresh DS18B20 reading is invalid and no retained valid interval exists, the production build falls back to `SKIMMERSENSE_NORMAL_TIMER_SECONDS`, currently 1800 s / 30 min.

For short event-only wakes that do not need to publish temperature, the firmware now skips the DS18B20 conversion and reuses the last valid adaptive `NORMAL` interval retained in RTC memory. This avoids forcing the schedule back to the conservative 30-minute fallback merely because temperature was intentionally not read on that wake.

The U-shaped profile intentionally spends the least energy in moderate water temperatures while increasing observation frequency again for very warm water and close to freezing.

The other production timings remain fixed:

- LOW confirmation: 300 s / 5 min continuously CLOSED
- WAIT_HIGH fallback: 1800 s / 30 min
- bounded Zigbee reconnect wait: 10 s

The adaptive timer is used only for healthy `NORMAL` operation. It does not modify `LOW_PENDING` or `WAIT_HIGH`.

## Hardware validation of adaptive timing

The adaptive profile has been tested on the real prototype at 24.50 C:

- production selected 3600 s / 1 h in `NORMAL`
- LOW closure still woke the XIAO immediately while the 1-hour timer was armed
- `LOW_PENDING` still used the fixed 300 s uninterrupted confirmation window
- confirmed LOW entered `WAIT_HIGH`
- the 1800 s WAIT_HIGH fallback woke and reported temperature without clearing the pending state
- HIGH opening woke immediately and returned to `NORMAL`
- the adaptive 3600 s interval was restored after returning to `NORMAL`

A later hardware run at 23.75 C selected 7200 s / 2 h, and an event-only float wake was observed with:

```text
Water temperature: skipped for this wake
Adaptive NORMAL timer: 7200 s (cached from 23.75 C)
Zigbee cycle intentionally skipped for this state transition.
```

This validates the RTC-retained adaptive interval and the selective DS18B20-read path on real hardware.

This validates the main low-power design principle: periodic reporting can be slowed substantially without degrading the event-driven water-level response.

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

## Zigbee sleepy-device timeout

The longest planned periodic sleep is now 6 hours, so the production firmware no longer relies on the shorter default child-aging timeout.

It configures:

```cpp
zigbeeConfig.nwk_cfg.zed_cfg.ed_timeout =
    ESP_ZB_ED_AGING_TIMEOUT_2048MIN;
```

That corresponds to about 34 hours, leaving substantial margin over a 6-hour sleep and over isolated failed reconnect/report cycles.

The normal Zigbee keep-alive configuration remains 10 seconds while the stack is active.

## Zigbee behavior and its energy impact

The current Arduino-ESP32 / ZBOSS stack crashes if the firmware mutates certain Zigbee attributes at runtime and lets automatic reporting process them.

The validated low-power workaround is therefore:

1. wake and read the sensor data needed for the current state transition
2. skip the DS18B20 conversion on event-only wakes when no fresh temperature report is required
3. preload temperature and float attributes before `Zigbee.begin()` when a Zigbee cycle is needed
4. reconnect with a bounded wait
5. send explicit zero-initialized reports for temperature and/or float states
6. keep Zigbee Power Configuration setup disabled
7. skip explicit battery reporting
8. wait briefly for delivery
9. return to deep sleep

During isolation, explicit `BatteryPercentageRemaining` reporting reproducibly asserted in the current ZBOSS version. `ZigbeeTempSensor::setPowerSource(...)` also caused a crash in this firmware configuration. MAX17048 voltage/SOC monitoring therefore remains local to the firmware/serial diagnostics for now.

Because Zigbee reconnect/report cycles are much more expensive than deep sleep, the adaptive schedule should reduce average consumption substantially versus the previous fixed 30-minute periodic refresh. Selective DS18B20 reads also remove the roughly 10-bit conversion/startup cost from short event-only wakes. Final autonomy must still be based on measured current rather than on wake-count reduction alone.

### Validated Zigbee active-time reduction

The production candidate originally kept deliberately conservative pauses around each reporting cycle:

```text
post-connect idle     8000 ms
between reports        400 ms
post-report wait       2000 ms
```

After staged hardware testing, both the anti-wave and production environments now use:

```text
post-connect idle     2000 ms
between reports        100 ms
post-report wait        750 ms
```

For a full cycle that sends temperature + LOW + HIGH, the fixed deliberate waits therefore change from:

```text
8000 + 3 x 400 + 2000 = 11200 ms
```

to:

```text
2000 + 3 x 100 + 750 = 3050 ms
```

This is a reduction of about 8.15 s, or roughly 73%, in fixed awake delay per complete reporting cycle. The actual total awake time is longer because Zigbee startup/reconnect and sensor work still take time.

The 10-second Zigbee reconnect timeout was intentionally left unchanged. It is only a maximum: the firmware exits the reconnect loop as soon as `Zigbee.connected()` becomes true, so reducing that timeout would not save energy during normal fast reconnects and would only reduce tolerance of a difficult reconnect.

Hardware validation included repeated 60-second anti-wave `NORMAL` cycles with temperature + LOW + HIGH reports, `WAIT_HIGH` temperature-only reporting, `WAIT_HIGH -> NORMAL` reporting with all three values, and a production cycle at 29.50 C. All tested reports were accepted by the stack (`queued OK`), the device returned to deep sleep normally, and no assertion or Guru Meditation was observed.

The optimization does not change radio power, endpoint layout, ZCL attribute types or Zigbee child-aging configuration. Those remain unchanged for v0.9.

## DS18B20 optimization

When a fresh temperature is required, the sequence is:

1. D2 HIGH powers the DS18B20
2. wait briefly for sensor startup
3. perform a 10-bit conversion
4. read the result
5. put D3 / DATA in high-impedance mode
6. drive D2 LOW before sleep

10-bit resolution is sufficient for pool-water monitoring and reduces awake time compared with 12-bit conversion.

The production firmware no longer performs this sequence on every wake. It first evaluates the float state and wake reason, then powers the DS18B20 only when the resulting cycle plan requests a fresh temperature report. Event-only transitions such as a short LOW-level wake can therefore return to sleep without powering or converting the probe.

The last valid temperature-derived `NORMAL` interval and its source temperature are retained in RTC memory across deep sleep. If an event-only wake returns to `NORMAL` without a fresh DS18B20 reading, that cached interval is reused. The cache is reset on a real cold boot/reset so stale data is not carried across a full restart.

Hardware evidence collected on the prototype includes a 23.75 C measurement selecting 7200 s, followed by a later event wake that logged `Water temperature: skipped for this wake` while retaining `Adaptive NORMAL timer: 7200 s (cached from 23.75 C)`.

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
2. a periodic adaptive `NORMAL` wake and report using the validated 2000 / 100 / 750 ms Zigbee timing
3. DS18B20 conversion
4. normal Zigbee reconnect/report cycle
5. LOW event wake followed by `LOW_PENDING` with DS18B20 intentionally skipped
6. LOW reopen wake during the 5-minute confirmation window with the cached adaptive interval reused
7. confirmed LOW report and `WAIT_HIGH` entry
8. 30-minute WAIT_HIGH fallback wake
9. HIGH event wake and final report
10. MAX17048 alert wake
11. failed Zigbee reconnect attempt
12. battery-only and USB-charging cases separately

For autonomy modelling, measure several representative adaptive intervals rather than assuming the old fixed 48 Zigbee refreshes/day. At 5-12 C, the 6-hour profile requests only four periodic `NORMAL` refreshes/day; at 24-28 C, it requests 24/day; at >=28 C or <3 C, it returns to 48/day.

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
