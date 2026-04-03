# esp-roomba-mqtt

ESP8266 MQTT Roomba controller with Home Assistant auto-discovery, WiFi config portal, and web control panel. Useful for hooking up old Roombas to Home Assistant.

## Features

* WiFi config portal — AP mode web UI with EEPROM-backed settings, no hardcoded credentials at runtime
* Home Assistant MQTT auto-discovery — vacuum entity + 15 sensors registered automatically on connect
* Web control panel at `http://roomba.local` — control, configure, and update firmware from a browser
* OTA updates — via PlatformIO, web upload, or emergency recovery firmware
* Drive and song playback via JSON MQTT topics
* Telnet debug interface (debug builds)

## Parts

* [ESP-01 or ESP12E](http://www.ebay.com/itm/121951859776) ESP8266 WiFi microcontroller ($3-4). Any ESP8266 module will work.
* [Small 3.3V switching step-down regulator](https://www.amazon.com/gp/product/B01MQGMOKI) ($1-2)
* 5kΩ & 10kΩ resistors for 5V→3.3V voltage divider on Roomba TX
* Some ~10kΩ pullup/pulldown resistors to set ESP8266 boot mode (GPIO0, GPIO15, EN)
* 3.3V FTDI cable for initial USB programming
* Wire or Mini-DIN connector for the Roomba's 7-pin Mini-DIN port

## Electronics

![esp-roomba-mqtt schematic. ESP-12E symbol by J. Dunmire in kicad-ESP8266. is licensed under the Creative Commons Attribution-ShareAlike 4.0 International License. To view a copy of this license visit http://creativecommons.org/licenses/by-sa/4.0/](doc/schematic.png)

### Connections

| ESP8266 pin | Direction | Roomba / other |
|---|---|---|
| GPIO15 | → 10kΩ → | GND |
| GPIO0 | → 10kΩ → | 3.3V |
| EN | → 10kΩ → | 3.3V |
| TX | → | Roomba RX (Mini-DIN pin 3) |
| RX | ← 10kΩ ← 5kΩ ← | Roomba TX (Mini-DIN pin 4) |
| GPIO14 | → | Roomba BRC (Mini-DIN pin 5) |
| 3.3V | ← | Voltage regulator 3.3V output |
| GND | ← | Voltage regulator GND |
| — | Regulator Vin ← | Roomba Vpwr (Mini-DIN pin 1 or 2) |
| — | Regulator GND ← | Roomba GND (Mini-DIN pin 6 or 7) |

### Voltage divider

The Roomba serial TX is 5V; the ESP8266 RX is 3.3V. Use a voltage divider: 5kΩ in series from Roomba TX to ESP RX, then 10kΩ from ESP RX to GND. Any two resistors above a few kΩ with a ~1:2 ratio will work.

## Getting Started

### 1. Create `src/secrets.h`

This file is `.gitignore`'d. Create it manually:

```cpp
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#define MQTT_PASSWORD "your-mqtt-password"
```

These credentials seed the EEPROM on first boot. After that, settings can be changed at runtime via the config portal or web interface without reflashing.

> **Note for recovery firmware:** `src/updater.cpp` reads `secrets.h` directly (not EEPROM) to ensure it can connect even with a corrupt config. Keep this file in sync with your actual WiFi password.

### 2. Adjust compile-time defaults in `src/config.h`

Review and update these before the first flash:

| Define | Default | Description |
|---|---|---|
| `HOSTNAME` | `"roomba_780"` | mDNS name and OTA target |
| `ROOMBA_MODEL` | `"Roomba 780"` | Model string in HA device info |
| `ROOMBA_FRIENDLY_NAME` | `"Roomba 780"` | Display name in HA |
| `MQTT_SERVER` | `"10.0.0.2"` | MQTT broker IP or hostname |
| `MQTT_PORT` | `1883` | MQTT broker port |
| `MQTT_USER` | `"homeassistant"` | MQTT username |
| `MQTT_DISCOVERY` | `"homeassistant"` | HA MQTT discovery prefix |
| `TIMEZONE` | `TZ_Europe_Madrid` | Timezone for NTP clock sync |

These values become the factory-reset defaults. Runtime changes via the portal override them.

### 3. Build and upload

The project uses [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

| Environment | Board | Upload | Logging | Use case |
|---|---|---|---|---|
| `esp01_via_usb_debug` | ESP-01 1MB | USB `/dev/ttyUSB0` | Yes | Development |
| `esp01_via_ota_debug` | ESP-01 1MB | OTA `roomba.local` | Yes | Development |
| `esp01_via_usb_prod` | ESP-01 1MB | USB `/dev/ttyUSB0` | No | Production |
| `esp01_via_ota_prod` | ESP-01 1MB | OTA (configured IP) | No | Production |
| `esp01_updater` | ESP-01 1MB | USB | — | Emergency recovery |
| `d1_mini` | D1 Mini / ESP12E | OTA `roomba.local` | Yes | Alternative board |

**First flash** (USB serial):
```
pio run -e esp01_via_usb_prod -t upload
```

**Subsequent updates** (OTA, device already running):
```
pio run -e esp01_via_ota_prod -t upload
```

## First Boot and Config Portal

On first boot with no saved config (or when WiFi is unreachable for more than 15 s on boot or 2 min at runtime), the device enters config portal mode:

1. It creates an open WiFi AP: **`roomba-setup-AABBCC`** (last 3 bytes of MAC)
2. Connect to that network and open **`http://192.168.4.1`** in a browser
3. Fill in WiFi credentials, MQTT settings, hostname, discovery prefix, and NTP servers
4. Click Save — the device restarts and connects normally
5. If left idle, the portal times out after **5 minutes** and restarts to retry

## Web Interface

Once connected, the device serves a web UI at `http://roomba.local` (or `http://<device-ip>`):

| Route | Description |
|---|---|
| `/` | Home / control panel |
| `/control` | Roomba command buttons |
| `/config` | Edit WiFi, MQTT, NTP settings |
| `/update` | Upload a firmware `.bin` file |

## Home Assistant Integration

### MQTT Auto-Discovery

On every MQTT connect, the device publishes discovery configs to Home Assistant. No manual entity configuration is required.

**Vacuum entity** (supports): `start`, `stop`, `pause`, `return_home`, `locate`, `clean_spot`, `send_command`

**Sensors** (15 total): battery level (%), charging (binary), signal strength (dBm), IP address, serial connected (binary), state, MAC address, SSID, battery temperature (°C), voltage (mV), current (mA), hostname, OI mode, charge (mAh), capacity (mAh)

All entities use the LWT topic for online/offline availability.

### MQTT Topics

Device ID format: `{MQTT_IDPREFIX}{6-byte-lowercase-mac}` — e.g. `roomba_780_aabbccddeeff`

| Topic | Direction | Description |
|---|---|---|
| `homeassistant/vacuum/{id}/command` | Subscribe | Control commands |
| `homeassistant/vacuum/{id}/state` | Publish (retained) | Status JSON |
| `homeassistant/vacuum/{id}/drive` | Subscribe | Drive command JSON |
| `homeassistant/vacuum/{id}/play_song` | Subscribe | Song playback JSON |
| `homeassistant/vacuum/{id}/LWT` | Publish | `online` / `offline` |

### Available Commands

Publish any of these strings to the `command` topic:

| Command | Action |
|---|---|
| `turn_on` / `start` / `clean` | Start cleaning |
| `turn_off` | Power off |
| `stop` / `pause` | Pause cleaning |
| `clean_spot` | Spot clean |
| `locate` | Play locator sound |
| `max_clean` | Max clean mode |
| `return_to_base` | Return to dock |
| `wake_up` | Pulse BRC pin to wake |

### State JSON

Published (retained) to the `state` topic:

```json
{
  "battery_level": 85,
  "cleaning": false,
  "returning": false,
  "docked": true,
  "charging": true,
  "chargingState": 2,
  "voltage": 16800,
  "current": -400,
  "charge": 2100,
  "capacity": 2500,
  "distance": 0,
  "batteryTemperature": 28,
  "chargingSourcesAvailable": 3,
  "OIMode": 3,
  "state": "docked",
  "ssid": "MyNetwork",
  "rssi": -65,
  "ip": "192.168.1.50",
  "hostname": "roomba",
  "mac": "AA:BB:CC:DD:EE:FF",
  "roomba_connected": true
}
```

### Drive Command

Publish JSON to the `drive` topic:

```json
{"velocity": 200, "radius": 32767}
```

* `velocity`: -500 to 500 mm/s
* `radius`: 32767 = straight, 1 = CCW spin in place, -1 = CW spin in place, -32768 = straight

### Song Playback

Publish a JSON array to the `play_song` topic. Bytes alternate between MIDI note number and duration (in 1/64 s units), following the [iRobot Open Interface spec](http://www.irobotweb.com/~/media/MainSite/PDFs/About/STEM/Create/iRobot_Roomba_600_Open_Interface_Spec.pdf). Maximum 128 bytes (64 notes):

```json
[60, 16, 62, 16, 64, 16]
```

## OTA Updates

Three ways to update firmware:

**1. PlatformIO OTA** (normal workflow):
```
pio run -e esp01_via_ota_prod -t upload
```

**2. Web upload**: Navigate to `http://roomba.local/update`, select a `.bin` file, and click Upload.

**3. Recovery firmware** — use when the main firmware is so broken it cannot connect to WiFi:
1. Make sure `src/secrets.h` has your current WiFi password
2. Flash recovery firmware via USB: `pio run -e esp01_updater -t upload`
3. Once it connects to WiFi, push the fixed main firmware via OTA: `pio run -e esp01_via_ota_prod -t upload`

The recovery firmware (`src/updater.cpp`) is a minimal sketch — just WiFi + ArduinoOTA — specifically designed to survive situations where the main firmware cannot be fixed over the air.

## RoombaFlasher Desktop Tool

`tools/flasher/` is a small GUI app (Rust + [egui](https://github.com/emilk/egui)) for flashing firmware over the device's HTTP endpoint. Useful as a standalone tool without requiring PlatformIO.

**Build** (requires Rust toolchain):
```bash
cd tools/flasher
cargo build --release             # Linux binary
bash build.sh                      # Linux + Windows cross-compile
```

**Usage**: enter the device hostname or IP, select (or bundle) `firmware.bin`, click Flash.

## Testing with Mosquitto

[Mosquitto](https://mosquitto.org/) can be used to test commands and monitor status. Replace `DEVICE_ID` with your device's actual ID (visible in HA or the web UI):

```bash
export MQTT_SERVER=your-broker-host
export MQTT_USER=homeassistant
export MQTT_PASSWORD=your-mqtt-password
export DEVICE_ID=roomba_780_aabbccddeeff

# Send a command
mosquitto_pub -t "homeassistant/vacuum/$DEVICE_ID/command" \
  -h $MQTT_SERVER -p 1883 -u $MQTT_USER -P $MQTT_PASSWORD -V mqttv311 \
  -m "turn_on"

# Subscribe to all device topics
mosquitto_sub -t "homeassistant/vacuum/$DEVICE_ID/#" -v \
  -h $MQTT_SERVER -p 1883 -u $MQTT_USER -P $MQTT_PASSWORD -V mqttv311
```

## Debugging

A telnet debug interface is included in `*_debug` builds (`-DLOGGING=1`). Connect with:

```
telnet roomba.local
```

Available debug commands (via the `debugCallback` function): `baud19200`, `baud115200`, `baud57600`, `baud38400`, `sleep5`, `wake`, `readadc`, `streamresume`, `streampause`, `stream`, `streamreset`, `time`, `version`, `rreset`, `quit`, plus all Roomba control commands.

## Firmware Version

Firmware version is auto-generated at build time as `MAJOR.MINOR.PATCH`, where `PATCH` is the git commit count. The version is injected by `tools/set_version.py` as a PlatformIO pre-build script.

## Roomba 650 Sleep on Dock Issue

Newer Roomba 650s (2016 and newer) fall asleep after ~1 minute of being on the dock. Though the [iRobot Create 2 docs](http://www.irobotweb.com/~/media/MainSite/PDFs/About/STEM/Create/iRobot_Roomba_600_Open_Interface_Spec.pdf) say that you can keep a Roomba awake by pulsing the BRC pin low, it doesn't seem to work for newer Roomba 650s when they are on the dock. [Thinking Cleaner's docs](http://www.thinkingcleaner.com/compatibility.html) note that this is likely a bug. There is a `ROOMBA_650_SLEEP_FIX` option in `src/config.h` that attempts a workaround, but it has not been confirmed to work reliably.
