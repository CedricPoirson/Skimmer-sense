# Home Assistant Integration

SkimmerSense is paired through Zigbee2MQTT and appears in Home Assistant as a sleepy Zigbee sensor device.

## Working entities

The current installation exposes:

- pool-water temperature
- low float raw state
- high float raw state
- Zigbee link quality

Current entity IDs include:

```text
sensor.0x10bda3fffe90a960_temperature
binary_sensor.0x10bda3fffe90a960_low_level_11
binary_sensor.0x10bda3fffe90a960_high_level_12
```

The hexadecimal prefix is device-specific. Do not copy these IDs blindly to another installation.

A battery entity may still exist in Zigbee2MQTT/Home Assistant from earlier interviews or test firmware. Do not rely on it for current v0.9 operation: Zigbee Power Configuration setup/reporting is intentionally disabled on the validated production candidate.

### Battery reporting limitation

On the current Arduino-ESP32 / ZBOSS stack, an explicit `BatteryPercentageRemaining` report reproducibly asserts in `esp_zigbee_zcl_command.c:263`, even when addressed directly to the coordinator. `ZigbeeTempSensor::setPowerSource(...)` also caused a reproducible ZBOSS crash in this firmware configuration.

Therefore:

- temperature and float reporting are considered validated
- MAX17048 voltage/SOC are still read locally by the firmware and shown in serial diagnostics
- Zigbee Power Configuration setup is disabled
- explicit Zigbee battery reporting is disabled
- reliable battery telemetry in Home Assistant is **not yet considered validated**
- do not remove/re-pair the Zigbee device just to work around this unless a future stack fix requires it

## Raw float semantics

Both float endpoints report the physical reed-contact state:

- `on` = contact CLOSED = float physically down
- `off` = contact OPEN = float physically up

With the low float mounted below the high float:

| Low | High | Meaning |
|---|---|---|
| `on` | `on` | water below low threshold |
| `off` | `on` | water between thresholds |
| `off` | `off` | water above high threshold |
| `on` | `off` | impossible / float fault |

This raw representation is intentional. Home Assistant derives the human-readable level state.

## Sensor-side anti-wave logic

The v0.9 production-candidate firmware already performs continuous level confirmation before publishing a refill request:

```text
NORMAL
  LOW closes
     |
     v
LOW_PENDING
  start 5-minute timer
  watch LOW for reopening
     |
     +-- LOW reopens before 5 min
     |      -> immediate GPIO wake
     |      -> reject wave / bather motion
     |      -> NORMAL
     |
     +-- LOW remains CLOSED continuously for 5 min
            + HIGH CLOSED
            -> publish ON/ON
            -> WAIT_HIGH

WAIT_HIGH
  LOW transitions ignored
  HIGH opens
     -> immediate GPIO wake
     -> publish final states
     -> NORMAL
```

Production timing is now split between an adaptive periodic `NORMAL` refresh and fixed safety/state-machine timers.

### Adaptive periodic refresh

| Water temperature | Periodic `NORMAL` wake |
|---|---:|
| >= 28 C | 30 min |
| 24 to < 28 C | 1 h |
| 18 to < 24 C | 2 h |
| 12 to < 18 C | 4 h |
| 5 to < 12 C | 6 h |
| 3 to < 5 C | 2 h |
| < 3 C | 30 min |

If the temperature measurement is invalid, firmware falls back to a 30-minute `NORMAL` timer.

Fixed timings remain:

- LOW confirmation: 5 min continuously CLOSED
- WAIT_HIGH fallback: 30 min

The adaptive periodic timer does **not** delay LOW or HIGH events. Float transitions are separate GPIO wake sources. This has been validated on the real prototype: with water at 24.50 C, `NORMAL` selected a 1-hour timer, LOW still woke immediately, the 5-minute confirmation worked unchanged, WAIT_HIGH used its 30-minute fallback, and HIGH opening woke immediately and returned to the 1-hour `NORMAL` interval.

The WAIT_HIGH fallback is deliberately long because a confirmed low-water request may remain pending for hours until the permitted overnight refill window. HIGH opening is still event-driven and wakes the sensor immediately, so the 30-minute fallback does not delay refill completion.

This means Home Assistant receives the confirmed `ON/ON` state only after the sensor-side 5-minute continuous confirmation has completed.

### Sleepy-device availability note

The longest periodic `NORMAL` sleep is now 6 hours. The firmware extends the Zigbee End Device aging timeout to `ESP_ZB_ED_AGING_TIMEOUT_2048MIN` (about 34 hours) so the device can remain a legitimate sleepy child across multi-hour sleeps.

Zigbee2MQTT/Home Assistant availability configuration should not be tuned so aggressively that a planned 4-hour or 6-hour sleep is misinterpreted as a fault. Long-duration field validation of availability behavior is still part of the v0.9 acceptance work.

### Avoid accidental double confirmation

If Home Assistant also requires `ON/ON` to remain stable for another 5 minutes before opening the valve, the effective start delay can approach **10 minutes**:

```text
5 min firmware confirmation + 5 min Home Assistant confirmation
```

This is safe but may be unnecessarily conservative. Keep it for initial commissioning if desired, then decide from real pool behavior whether the Home Assistant delay should be shortened once the hardware installation is complete.

## Template level-state sensor

Example template used on the current installation:

```yaml
template:
  - sensor:
      - name: "État niveau piscine"
        unique_id: etat_niveau_piscine
        state: >
          {% set low = states('binary_sensor.0x10bda3fffe90a960_low_level_11') %}
          {% set high = states('binary_sensor.0x10bda3fffe90a960_high_level_12') %}
          {% set remplissage = is_state('switch.relais6', 'on') %}

          {% if low not in ['on', 'off'] or high not in ['on', 'off'] %}
            Indisponible
          {% elif low == 'on' and high == 'off' %}
            Défaut flotteurs
          {% elif remplissage %}
            Remplissage
          {% elif low == 'on' and high == 'on' %}
            Niveau bas
          {% else %}
            Niveau correct
          {% endif %}

        icon: >
          {% set low = states('binary_sensor.0x10bda3fffe90a960_low_level_11') %}
          {% set high = states('binary_sensor.0x10bda3fffe90a960_high_level_12') %}

          {% if low not in ['on', 'off'] or high not in ['on', 'off'] %}
            mdi:water-alert
          {% elif low == 'on' and high == 'off' %}
            mdi:alert
          {% elif is_state('switch.relais6', 'on') %}
            mdi:water-pump
          {% elif low == 'on' and high == 'on' %}
            mdi:waves-arrow-down
          {% else %}
            mdi:waves
          {% endif %}
```

After editing `configuration.yaml`, validate the configuration before restarting Home Assistant.

## Refill control

The refill valve is controlled separately from SkimmerSense. The intended fixed-side chain is:

```text
Home Assistant -> switch.relais6 -> IPX800 V4 relay -> 24 VAC -> normally-closed Rain Bird valve
```

The sensor node never directly energizes the valve.

The Home Assistant/IPX800 **logic and dry-contact relay behavior have been tested**, but the final real solenoid/water path has not yet completed commissioning.

A confirmed low-water request does not imply that water starts immediately. Home Assistant may keep that request pending until the allowed overnight refill window.

## Refill state machine

The intended behavior is:

```text
LOW=ON, HIGH=ON
        |
        | confirmed low-water request
        | wait for allowed overnight window if necessary
        v
     REFILL ON
        |
        | LOW=OFF, HIGH=ON
        | keep filling
        v
LOW=OFF, HIGH=OFF
        |
        | high-level completion
        v
     REFILL OFF
```

`LOW=ON, HIGH=OFF` is physically inconsistent and must force the valve OFF.

## Current safety strategy

The tested Home Assistant logic uses the following safeguards:

- automatic refill only during the allowed overnight window
- low-level state validation before opening the valve
- continue filling through the middle hysteresis state
- high-level completion closes the valve
- impossible float state -> immediate valve close + alert
- lost/unavailable float sensor during filling -> valve close
- Home Assistant maximum continuous valve-open timeout
- hard stop at the end of the allowed refill window
- Home Assistant restart -> valve forced OFF
- relay turning on outside the permitted window -> valve forced OFF
- independent IPX800 relay timer as a second maximum-open limit

The current design uses a shorter Home Assistant timeout than the IPX800 timeout so Home Assistant normally stops the valve first while the IPX800 remains an independent fallback.

## Recommended automation structure

For the final production configuration, separating control and safety remains preferable to putting every trigger in one large `mode: restart` automation:

1. refill controller for normal start / continue / stop behavior
2. safety automation for impossible state, unavailable sensors, timeout and forbidden-time relay activation

This prevents a relay-state trigger from restarting or interrupting the same automation that just turned the relay on.

## Dashboard card

A simple working Lovelace card is:

```yaml
type: entities
show_header_toggle: false
entities:
  - entity: sensor.etat_niveau_piscine
    name: État du niveau
  - entity: sensor.0x10bda3fffe90a960_temperature
    name: Température de l'eau
  - entity: switch.relais6
    name: Remplissage automatique
```

For daily use, the two raw float entities can stay off the main dashboard. Keep them available in developer tools or a diagnostics view.

## Commissioning

Before opening the real water supply:

1. verify the production SkimmerSense profile is installed
2. test all four float-state combinations with the manual water shutoff closed
3. verify the firmware-side 5-minute continuous anti-wave confirmation
4. verify that reopening LOW during those 5 minutes cancels the request immediately
5. verify that a long adaptive `NORMAL` timer does not delay LOW GPIO wake
6. verify that HIGH GPIO wake remains immediate in WAIT_HIGH
7. check Zigbee2MQTT/Home Assistant availability behavior across the longest planned sleep interval
8. decide whether the additional Home Assistant low-level delay should remain at 5 minutes for the first real tests
9. verify the Home Assistant maximum-open timeout
10. verify the independent IPX800 timeout
11. verify the valve is normally closed when 24 VAC is absent
12. perform the first real refill cycle under supervision
13. measure the real refill duration between the two float thresholds
14. use that duration to confirm or tighten the final timeout values
