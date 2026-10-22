# ESP32 TCU SMS web flasher

This is a static site for flashing the two targets from the parent PlatformIO
project. It uses `esp-web-install-button` and requires Chrome or Edge over HTTPS
(or localhost).

## Release files

Place these files beside this directory when publishing the site:

```text
firmware/esp32-c3-sim800.merged.bin
firmware/t-call.merged.bin
```

The merged images must be built from the parent project targets:

```text
pio run -e esp32-c3-devkitm-1
pio run -e esp32dev
```

Release 2.2.0 adds automatic Wi-Fi/GPRS fallback, incoming SMS polling and the
FSO HTTP relay. On TTGO T-Call, Wi-Fi is temporarily disabled during SIM800
transmission and the following SMS receive window to avoid board resets.

The page intentionally flashes firmware only. Configuration remains managed by
the device's own setup page; on first boot the firmware generates its device ID
and AES key in LittleFS.
