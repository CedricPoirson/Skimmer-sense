# Home Assistant Integration

SkimmerSense is paired through Zigbee2MQTT and appears in Home Assistant as a sleepy Zigbee sensor device.

## Working entities

The current installation exposes:

- pool-water temperature
- low float raw state
- high float raw state
- Zigbee link quality
- a Power Configuration / battery value may also be exposed by Zigbee2MQTT from the preloaded endpoint attributes

Current entity IDs include:

```text
sensor.0x10bda3fffe90a960_temperature
binary_sensor.0x10bda3fffe90a960_low_level_11
binary_sensor.0x10bda3fffe90a960_high_level_12
sensor.0x10bda3fffe90a960_battery
```

The hexadecimal prefix is device-specific. Do not copy these IDs blindly to another installation.

### Battery reporting limitation

The firmware preloads the standard Zigbee Power Configuration attributes before `Zigbee.begin()`, but **explicit battery percentage reporting is currently disabled**.

On the current Arduino-ESP32 / ZBOSS stack, an explicit `BatteryPercentageRemaining` report reproducibly asserts in `esp_zigbee_zcl_command.c:263`, even when addressed directly to the coordinator.

Therefore:

- temperature and float reporting are considered validated
- battery percentage may appear from interview/preloaded attributes
- reliable periodic battery refresh is **not yet considered validated**
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

The v0.9 production-candidate firmware already performs level confirmation before publishing a refill request:

```text
NORMAL
  LOW closes
     |
     | 5 min timer-only confirmation
     v
LOW_PENDING
  LOW reopened -> reject wave / bather motion
  LOW still closed + HIGH closed -> publish ON/ON
     |
     v
WAIT_HIGH
  LOW transitions ignored
  HIGH opens -> publish final states and return NORMAL
```

Production timing is currently:

- normal periodic refresh: 30 min
- LOW confirmation: 5 min
- WAIT_HIGH fallback: 2 min

This means Home Assistant receives the confirmed `ON/ON` state only after the sensor-side confirmation has completed.

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

## Refill state machine

The intended behavior is:

```text
LOW=ON, HIGH=ON
        |
        | confirmed low-water request
        v
     REFILL ON
        |
        | LOW=OFF, HIGH=ON
        | keep filling
        v
LOW=OFF, HIGH=OFF
        |
        | stable high-level completion
        v
     REFILL OFF
```

`LOW=ON, HIGH=OFF` is physically inconsistent and must force the valve OFF.

## Current safety strategy

The tested Home Assistant logic uses the following safeguards:

- automatic refill only during the allowed overnight window
- low-level state validation before opening the valve
- continue filling through the middle hysteresis state
- high-level state validation before normal completion
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
3. verify the firmware-side 5-minute anti-wave confirmation
4. decide whether the additional Home Assistant low-level delay should remain at 5 minutes for the first real tests
5. verify the Home Assistant maximum-open timeout
6. verify the independent IPX800 timeout
7. verify the valve is normally closed when 24 VAC is absent
8. perform the first real refill cycle under supervision
9. measure the real refill duration between the two float thresholds
10. use that duration to confirm or tighten the final timeout values
