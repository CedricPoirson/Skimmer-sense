# Power Consumption Notes

SkimmerSense is designed as a battery-powered sleepy Zigbee end device.

## Initial strategy

- Keep the ESP32-C6 in deep sleep most of the time
- Wake periodically every 30-60 minutes
- Power the DS18B20 only during a temperature measurement
- Use passive reed float switches with GPIO wake-up
- Keep the Zigbee radio active only long enough to report measurements/events
- Use the MAX17048 in its low-power mode

## Battery target

Initial battery: protected 18650 Li-ion, approximately 3500 mAh.

The first objective is not to predict multi-year battery life from component datasheets, but to measure real current in these states:

1. Deep sleep
2. Sensor measurement
3. Zigbee join
4. Normal Zigbee report
5. Float-switch wake-up

## Validation plan

After firmware bring-up, record real current and duty cycle, then calculate expected battery life from measured values. Real-world self-discharge, temperature and radio retries should be included in the final estimate.
