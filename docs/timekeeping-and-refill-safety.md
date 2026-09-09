# Timekeeping and refill-safety policy

SkimmerSense is designed first as a low-power, safety-oriented level sensor. The production firmware therefore does **not** keep Wi-Fi active and does **not** wake only to perform NTP time synchronization.

## Design decision

Wall-clock scheduling belongs to Home Assistant, which is mains-powered, already knows local time, and controls the fixed IPX800 / normally-closed refill valve.

The battery-powered XIAO ESP32-C6 only needs monotonic timing for its own safety state machine:

- continuous LOW confirmation;
- WAIT_HIGH fallback checks;
- temperature-dependent periodic refreshes;
- Zigbee retry delays;
- retained critical refill-completion state.

These functions use deep-sleep timers and RTC-retained state. They do not require calendar time or an NTP request.

## Why not NTP in normal operation?

Starting Wi-Fi, associating to the access point and requesting NTP would cost much more energy than keeping the ESP32-C6 asleep and using the RTC/deep-sleep timer. More importantly, time synchronization must never become a dependency for detecting a low level or stopping a refill.

SERVICE mode may use Wi-Fi while the D6/GPIO16 maintenance jumper is fitted, but normal production operation remains Zigbee + deep sleep only.

If standalone wall-clock scheduling is ever required in the sensor itself, the preferred future design is an **opportunistic synchronization** during an already-required radio wake, with the last valid reference retained locally. It must not create additional periodic Wi-Fi wake-ups and must never gate the HIGH-level safety stop.

## Zigbee reporting policy

Home Assistant should receive **validated control events**, not every physical bounce or wave-induced transition.

### LOW / refill start

1. LOW closes.
2. The ESP enters `LOW_PENDING`.
3. LOW must remain continuously closed for the complete production confirmation window (5 minutes).
4. If LOW reopens before the end of the window, the event is rejected as wave/bather motion and **nothing is published as a refill request**.
5. Only a continuously confirmed LOW condition is published and may allow Home Assistant to start a refill.

This prevents transient water motion from becoming a false positive in Home Assistant.

### HIGH / refill stop

While waiting for the refill to finish, LOW transitions are intentionally ignored and HIGH is the safety-critical input.

When HIGH reaches the refill-complete state:

- the GPIO wake is immediate;
- the final LOW/HIGH snapshot is retained in RTC memory before radio transmission;
- the final float state is sent repeatedly in the same Zigbee wake window;
- if transmission cannot complete, the retained completion event is retried;
- Home Assistant must close the normally-closed valve on the first valid refill-complete report.

A failed time synchronization, unavailable wall clock, or missed periodic refresh must never delay this HIGH stop path.

## Sensor-fault policy

A physically impossible float combination, currently `LOW=CLOSED / HIGH=OPEN`, is a fault rather than a refill request.

The safe system reaction is:

- do not start a new refill;
- if the valve is already open, close it;
- surface a sensor-fault condition to Home Assistant;
- notify the user;
- require a healthy, coherent float state before automatic refill is allowed again.

Future firmware may expose a dedicated Zigbee fault entity. Until then, Home Assistant must treat the impossible LOW/HIGH combination as a fault state rather than normal level information.

## Safety hierarchy

The intended hierarchy is:

1. **HIGH refill-complete detection** — immediate stop path, highest priority.
2. **Impossible/incoherent sensor state** — fail closed and report a fault.
3. **Continuously validated LOW** — may request a refill.
4. **Transient LOW / waves / swimmer motion** — ignored, no refill request.
5. **Periodic temperature/battery telemetry** — useful but never safety-critical.
6. **Wall-clock synchronization** — optional convenience only; Home Assistant remains the scheduling authority.

This keeps SkimmerSense low-power while avoiding false positives and preserving a deterministic fail-safe stop path.
