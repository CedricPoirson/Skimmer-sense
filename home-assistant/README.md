# Home Assistant Integration

SkimmerSense is currently paired through Zigbee2MQTT and appears in Home Assistant as a Zigbee sensor device.

## Working entities

The prototype exposes:

- pool-water temperature
- low float raw state
- high float raw state
- Zigbee link quality

Battery entities will be added after the MAX17048 has been validated with the final 18650 installed.

On the current test installation, Zigbee2MQTT generated entity IDs similar to:

```text
sensor.0x10bda3fffe90a960_temperature
binary_sensor.0x10bda3fffe90a960_low_level_11
binary_sensor.0x10bda3fffe90a960_high_level_12
```

The hexadecimal prefix is device-specific. Do not copy these IDs blindly to another installation.

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

The refill valve is controlled separately from SkimmerSense. On the current installation the fixed-side chain is:

```text
Home Assistant -> switch.relais6 -> IPX800 V4 relay -> 24 VAC -> normally-closed Rain Bird valve
```

The sensor node never directly energizes the valve.

## Validated refill state machine

The intended behavior is:

```text
LOW=ON, HIGH=ON
        |
        | stable low-water delay
        v
     REFILL ON
        |
        | LOW=OFF, HIGH=ON
        | keep filling
        v
LOW=OFF, HIGH=OFF
        |
        | stable high-level delay
        v
     REFILL OFF
```

`LOW=ON, HIGH=OFF` is physically inconsistent and must force the valve OFF.

## Current safety strategy

The tested Home Assistant logic uses the following safeguards:

- automatic refill only during a quiet overnight window
- require the low-water state to remain stable before opening the valve
- continue filling through the middle hysteresis state
- require the high-water state to remain stable before closing after normal completion
- impossible float state -> immediate valve close + alert
- lost/unavailable float sensor during filling -> valve close
- Home Assistant maximum continuous valve-open timeout
- hard stop at the end of the allowed refill window
- Home Assistant restart -> valve forced OFF
- relay turning on outside the permitted window -> valve forced OFF
- independent IPX800 relay timer as a second maximum-open limit

The current installation uses a shorter Home Assistant timeout than the IPX800 timeout so Home Assistant normally stops the valve first while the IPX800 remains an independent fallback.

## Recommended automation structure

For a production configuration, prefer separating concerns rather than putting every trigger in one large `mode: restart` automation:

1. a refill controller for the normal start/continue/stop state machine
2. a safety automation for impossible state, unavailable sensors, timeout and forbidden-time relay activation

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

1. disable any accelerated test automation
2. enable only the production automation(s)
3. test all four float-state combinations with the manual water shutoff closed
4. verify the Home Assistant timeout
5. verify the independent IPX800 timeout
6. verify the valve is normally closed when 24 VAC is absent
7. perform the first real refill cycle under supervision
8. measure the real refill duration between the two float thresholds

That measured duration should be used to confirm or tighten the final safety timeout.
