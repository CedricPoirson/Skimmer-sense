# SkimmerSense SERVICE mode

SERVICE mode provides local diagnostics and Wi-Fi OTA while the device is powered from its normal battery. It is intentionally isolated from the Zigbee runtime: Zigbee is not started and deep sleep is disabled while SERVICE mode is active.

## Hardware jumper

The SERVICE input is **XIAO D6 / ESP32-C6 GPIO16**.

Wire a 2-pin header so a jumper can short:

```text
D6 (GPIO16) ----- jumper ----- GND
```

Normal production operation: **jumper open**.

Maintenance operation: **jumper closed**, then press RESET or power-cycle the XIAO.

GPIO16 is not one of the ESP32-C6 RTC GPIOs (RTC/EXT1 wake is limited to GPIO0..GPIO7), so closing this jumper while the XIAO is already in deep sleep does **not** wake it. A reset/power-cycle is required after fitting the jumper.

## Connecting without USB

With the jumper closed at boot, SkimmerSense creates its own Wi-Fi access point. No home Wi-Fi credentials are stored or required.

The SSID is:

```text
SkimmerSense-XXXXXX
```

The password is derived directly from the same six-character suffix:

```text
SKM-XXXXXX
```

Example:

```text
SSID:     SkimmerSense-A1B2C3
Password: SKM-A1B2C3
```

Connect a phone or laptop to that access point, then open:

```text
http://192.168.4.1/
```

## Information available

The service page currently shows:

- firmware version/flavor;
- retained pool-level state;
- current LOW and HIGH float states;
- current DS18B20 water temperature;
- MAX17048 voltage, raw SOC, rounded percentage and INT state;
- cached adaptive NORMAL sleep interval and temperature;
- free heap;
- ESP reset reason;
- retained diagnostic cycle information;
- the eight most recent **non-deep-sleep resets** stored in NVS.

Normal timer/GPIO deep-sleep wakes do not write the reset history to flash, so ordinary operation does not create continuous NVS wear. Power-on, manual/software reset, panic, watchdog and brownout resets are retained for later inspection.

The first implementation mainly identifies the class of reset. More detailed execution-stage breadcrumbs can be added after the SERVICE/OTA path is hardware-validated.

## OTA update

The existing `zigbee.csv` partition layout contains `otadata`, `ota_0` and `ota_1`, so the service page can upload a normal PlatformIO firmware binary to the inactive application slot.

Build the production image with:

```bash
cd firmware
pio run -e seeed_xiao_esp32c6_sleep_zigbee_production
```

The file to select in the SERVICE web page is:

```text
.pio/build/seeed_xiao_esp32c6_sleep_zigbee_production/firmware.bin
```

The OTA update writes the application slot only. It does **not** intentionally erase the separate Zigbee storage partitions, so the normal OTA path is designed to preserve Zigbee pairing.

After the page reports a successful upload:

1. remove the D6-GND SERVICE jumper;
2. press the web-page reboot button;
3. verify that the new firmware reconnects to the existing Zigbee network without re-pairing.

Do not use `pio run -t erase` as part of normal updating, because that would erase persistent Zigbee data.

## Battery note

SERVICE mode keeps the ESP32-C6 awake with Wi-Fi enabled. Its consumption is therefore much higher than normal deep-sleep operation. It is intended for short maintenance sessions, not unattended battery operation.
