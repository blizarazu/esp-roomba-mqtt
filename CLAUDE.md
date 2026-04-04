# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP8266 firmware to integrate a Roomba vacuum with Home Assistant via MQTT. Targets ESP-01 (1 MB flash) and ESP12E/D1 Mini boards. Uses PlatformIO as the build system.

## Build Commands

```bash
# First-time USB flash (production)
pio run -e esp01_via_usb_prod -t upload

# OTA update to running device
pio run -e esp01_via_ota_prod -t upload

# Debug build via USB
pio run -e esp01_via_usb_debug -t upload

# Build only (no upload)
pio run -e esp01_via_usb_prod

# Build Rust flasher GUI tool
cd tools/flasher && cargo build --release
```

### PlatformIO Environments

| Environment | Upload Method | Debug |
|---|---|---|
| `esp01_via_usb_prod` | USB `/dev/ttyUSB0` | No |
| `esp01_via_ota_prod` | OTA `roomba.local` | No |
| `esp01_via_usb_debug` | USB | Yes (telnet on port 23) |
| `esp01_via_ota_debug` | OTA | Yes |
| `esp01_updater` | USB | Recovery firmware |
| `d1_mini` | OTA | Yes |

## Required Setup

Create `src/secrets.h` (not in git) before first build:
```cpp
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#define MQTT_PASSWORD "your-mqtt-password"
```

These values seed EEPROM on first boot. After that, credentials are runtime-configurable via the web portal. The `esp01_updater` environment reads `secrets.h` directly to bypass EEPROM.

## Architecture

### Core Files

- `src/main.cpp` (~1800 lines) — all firmware logic: WiFi/MQTT, web server, sensor parsing, state machine
- `src/config_manager.cpp/h` — EEPROM config storage (404-byte struct with XOR checksum), config portal HTML
- `src/config.h` — compile-time defaults (hostname, MQTT broker, GPIO pins, timing constants)
- `lib/Roomba/` — Roomba Open Interface protocol library (sensor IDs, opcodes, serial streaming)
- `tools/set_version.py` — PlatformIO pre-build script; injects `FIRMWARE_VERSION` as `MAJOR.MINOR.PATCH` where PATCH = git commit count
- `tools/flasher/` — standalone Rust/egui GUI app for OTA firmware uploads without PlatformIO

### Loop Structure

`loop()` runs these in order every cycle:
1. `ArduinoOTA.handle()` — mandatory; OTA updates fail if skipped
2. Telnet debug handler (debug builds only)
3. Config portal handler (when in AP mode)
4. `controlServer.handleClient()` — HTTP requests
5. WiFi watchdog → triggers config portal on 15s+2min timeout
6. MQTT reconnect (5s interval)
7. Sensor discovery publish (120s interval)
8. Status publish (10s active / 30s idle)
9. `readSensorPacket()` — parse Roomba sensor stream
10. `mqttClient.loop()`

### MQTT Topics

Base path: `homeassistant/vacuum/{HOSTNAME}/`

- `command` (subscribe) — turn_on, stop, dock, locate, start_pause, return_to_base, clean_spot
- `state` (publish retained) — JSON with full device state
- `drive` (subscribe) — `{"velocity": mm/s, "radius": units}`
- `play_song` (subscribe) — `[note1, duration1, note2, ...]`

### Web Server Endpoints

Served at `http://roomba.local` on port 80:
- `/` — status dashboard
- `/control` — control buttons + directional drive
- `/config` — WiFi/MQTT/NTP settings form
- `/update` — `.bin` firmware upload
- `/cmd` — POST Roomba commands
- `/drive` — POST `{"velocity": N, "radius": N}`

### Config Portal

When WiFi fails (15s+2min timeout), device starts AP `roomba-setup-AABBCC`. Navigate to `192.168.4.1` to configure. Auto-closes after 5 minutes.

### Roomba State Inference

State is derived from sensor stream data:
- **cleaning**: `current < -400 mA` (motor load)
- **docked**: `current > -50 mA` (idle on dock)
- **returning**: set by `returnToBase()` command, cleared on dock detection

### Debug Build Features

`-DLOGGING=1` enables RemoteDebug (telnet port 23). Connect with `telnet roomba.local`. Supports commands: `baud19200`, `baud115200`, `stream`, `streamreset`, `readadc`, `wake`, `rreset`, `time`, `version`, plus all Roomba control commands.

### Memory Constraints

Target is ESP-01 with 1 MB flash. Key limits:
- `MQTT_MAX_PACKET_SIZE=1024` for JSON discovery messages
- Production builds use `-Os` (optimize for size)
- Web server HTML is rendered inline as C++ strings to avoid SPIFFS
