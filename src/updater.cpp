// Minimal recovery firmware for the esp01_updater PlatformIO environment.
//
// Purpose: if the main firmware (main.cpp) has a bug that prevents it from
// booting or connecting to WiFi, OTA updates become impossible. In that case,
// flash this sketch via USB to restore OTA access, then use OTA to push the
// fixed main firmware without needing physical access to the device again.
//
// Usage:
//   1. Connect the ESP8266 via USB and flash: pio run -e esp01_updater -t upload
//   2. The device will connect to WiFi and listen for OTA updates.
//   3. Flash the main firmware via OTA: pio run -e esp01_via_ota_prod -t upload
//
// NOTE: This firmware intentionally uses hardcoded credentials from secrets.h
// rather than reading from EEPROM. This ensures recovery works even when the
// EEPROM config is corrupt or was written by a different firmware version.
// If the WiFi credentials were changed at runtime via the config portal, update
// secrets.h with the new credentials before flashing this recovery firmware.

#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <config.h>
#include <secrets.h>

void setup()
{
    String hostname(HOSTNAME);
    WiFi.hostname(hostname);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    ArduinoOTA.setHostname((const char *)hostname.c_str());
    ArduinoOTA.begin();
}

void loop()
{
    ArduinoOTA.handle();
}