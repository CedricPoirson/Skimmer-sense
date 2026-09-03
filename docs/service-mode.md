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

## Wi-Fi architecture

SERVICE mode uses **AP + STA simultaneously** when private home Wi-Fi credentials are present:

- the SkimmerSense always starts a private fallback access point;
- if home Wi-Fi credentials are present at build time, it also tries to join that network for up to 12 seconds;
- when the home connection succeeds, the HTTP diagnostics/OTA server is reachable from the LAN and through mDNS;
- if the home Wi-Fi is unavailable, the fallback AP remains usable.

The fallback AP is always:

```text
SSID:     SkimmerSense-XXXXXX
Password: SKM-XXXXXX
Address:  http://192.168.4.1/
```

When the home connection succeeds, SERVICE mode prints its DHCP address on the serial console and starts mDNS as:

```text
http://skimmersense.local/
```

Normally there is therefore no need to know the DHCP address. If `.local` name resolution is unavailable on a client, obtain the SkimmerSense address from the router/DHCP table or use the fallback AP at `192.168.4.1`.

The ESP32-C6 Wi-Fi radio is 2.4 GHz. The configured SSID therefore needs a 2.4 GHz network (or a combined SSID that accepts 2.4 GHz clients).

## Home Wi-Fi credentials

Credentials must **not** be committed to GitHub.

A safe template is tracked in the repository:

```text
firmware/include/wifi_secrets.example.h
```

Create the local file:

```bash
cd firmware/include
cp wifi_secrets.example.h wifi_secrets.h
```

Then edit `wifi_secrets.h`:

```cpp
#pragma once

#define SKIMMERSENSE_WIFI_SSID "Your-2.4GHz-WiFi"
#define SKIMMERSENSE_WIFI_PASSWORD "Your-WiFi-Password"
```

The real file is listed in `.gitignore`:

```text
firmware/include/wifi_secrets.h
```

If that file is absent, the firmware still builds and SERVICE mode simply uses the private fallback AP only. This is also how GitHub Actions builds the project without access to private Wi-Fi credentials.

## Connecting without USB

1. Fit the D6-GND SERVICE jumper.
2. Press RESET or power-cycle the XIAO.
3. If the configured home Wi-Fi is reachable, open `http://skimmersense.local/`.
4. Otherwise connect a phone/laptop to `SkimmerSense-XXXXXX` and open `http://192.168.4.1/`.

The home Wi-Fi password is never displayed by the service page. The fallback AP password is derived locally from the device suffix and is displayed because it is intentionally the recovery path.

## Information available

The service page currently shows:

- firmware version/flavor;
- fallback AP information;
- retained pool-level state;
- current LOW and HIGH float states;
- current DS18B20 water temperature;
- MAX17048 voltage, raw SOC, rounded percentage and INT state;
- cached adaptive NORMAL sleep interval and temperature;
- free heap;
- ESP reset reason;
- retained diagnostic cycle information;
- the eight most recent **non-deep-sleep resets** stored in NVS;
- a live Wi-Fi log page at `/logs`;
- a downloadable text log at `/logs-download`;
- the last production-cycle trace retained in RTC no-init memory, including
  wake cause, state, sensors, decision, Zigbee outcome and planned sleep.

Normal timer/GPIO deep-sleep wakes do not write the reset history to flash, so ordinary operation does not create continuous NVS wear. Power-on, manual/software reset, panic, watchdog and brownout resets are retained for later inspection.

The production-cycle trace is also flash-write-free. It is marked **complete**
only immediately before deep sleep; if the cycle stops earlier, SERVICE mode
shows it as **INCOMPLETE**. Fit the SERVICE jumper and press RESET without
removing power to preserve this RTC no-init trace. A full power loss or USB
flash may clear it. SERVICE-session events are kept in RAM and refresh in the
browser every two seconds.

## OTA update

SERVICE mode uses the repository partition table `firmware/skimmersense_zigbee_ota.csv`. It keeps the standard Arduino Zigbee persistent-storage addresses (`zb_storage`, `zb_fct` and `coredump`) unchanged, but enlarges both OTA application slots to `0x170000` bytes each. The SPIFFS area is reduced and is not used by SkimmerSense.

This custom layout is required because adding Wi-Fi, WebServer, mDNS and OTA support makes the combined Zigbee + SERVICE firmware larger than the standard Arduino `zigbee.csv` OTA slot.

### First installation of SERVICE mode

The partition table itself cannot be replaced by a normal application OTA. Therefore the first SERVICE-capable firmware installation must be flashed once over USB so PlatformIO also writes the new partition table. This operation is designed to keep the Zigbee storage partitions at the same flash addresses; nevertheless the first hardware test must verify that the existing Zigbee pairing survives before relying on OTA in production.

Do **not** erase the flash.

Build and upload the production image with:

```bash
cd firmware
pio run -e seeed_xiao_esp32c6_sleep_zigbee_production -t upload
```

After that first USB installation, future application updates can use the SERVICE web page.

Build the OTA image with:

```bash
cd firmware
pio run -e seeed_xiao_esp32c6_sleep_zigbee_production
```

The file to select in the SERVICE web page is:

```text
.pio/build/seeed_xiao_esp32c6_sleep_zigbee_production/firmware.bin
```

The OTA update writes the inactive application slot only. It does **not** intentionally erase the separate Zigbee storage partitions, so the normal OTA path is designed to preserve Zigbee pairing.

After the page reports a successful upload:

1. remove the D6-GND SERVICE jumper;
2. press the web-page reboot button;
3. verify that the new firmware reconnects to the existing Zigbee network without re-pairing.

Do not use `pio run -t erase` as part of normal updating, because that would erase persistent Zigbee data.

## Battery note

SERVICE mode keeps the ESP32-C6 awake with Wi-Fi enabled. AP+STA and the web server consume far more power than normal deep sleep. SERVICE mode is intended for short maintenance sessions only.
