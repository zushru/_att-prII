# Biometric Attendance System

Fingerprint attendance for a classroom, running on an ESP32. Students scan a finger, the device logs it, and teachers manage everything from a dashboard served over the device's own Wi-Fi. No internet or USB console needed.

## What it does

- Fingerprint attendance via an AS608 sensor
- Offline timekeeping with a DS3231 RTC
- Wi-Fi access point + web dashboard (admin PIN login, first-run setup)
- Enroll students, manage the roster, view daily/monthly logs
- Backup and restore, plus SF2 export support
- OLED status display and buzzer feedback

## Hardware

| Part | Pins (ESP32) |
| --- | --- |
| AS608 fingerprint sensor | RX 16, TX 17, TCH 4 |
| DS3231 RTC + SSD1306 OLED (I2C) | SDA 21, SCL 22 |
| Buzzer | GPIO 23 |

Board: `esp32dev`, 4 MB flash.

## Build

```sh
pio run -e att
pio run -e att -t upload
```

Needs [PlatformIO](https://platformio.org/). Partition layout is in `partitions/att.csv`.

## Notes

- The build runs `scripts/build/apply_pinned_webserver_patch.py`, which patches the Arduino WebServer parser temporarily and restores it after. Don't remove it.
- The dashboard is prebuilt into `src/dashboard_html.h` (gzipped). The editable HTML source isn't in this repo, so the dashboard can't be regenerated from here.

## Layout

- `src/`: `main.cpp` and the embedded dashboard
- `lib/core/`: attendance logic, roster, logs, auth, backup/restore
- `lib/hal_esp32/`: hardware drivers and HTTP handlers
- `lib/as608/`: fingerprint sensor code
- `lib/littlefs_file_set/`: LittleFS storage
- `docs/sf2/reference/`: SF2 reference workbook
