#pragma once
#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>

// Bump CONFIG_VERSION whenever the struct layout changes — triggers a factory reset
#define CONFIG_MAGIC   0xDEAD1234UL
#define CONFIG_VERSION 1

struct DeviceConfig {
    uint32_t magic;
    uint8_t  version;
    char     wifi_ssid[33];      // max 32 + null
    char     wifi_password[65];  // max 64 + null
    char     mqtt_server[65];    // IP or hostname, max 64 + null
    uint16_t mqtt_port;
    char     mqtt_user[33];      // max 32 + null
    char     mqtt_password[65];  // max 64 + null
    char     hostname[33];       // mDNS / OTA / HA hostname, max 32 + null
    char     mqtt_discovery[33]; // discovery prefix, e.g. "homeassistant"
    char     ntp_server1[33];
    char     ntp_server2[33];
    uint32_t checksum;           // XOR fold of all preceding bytes
};
// ~404 bytes total, well within the 4096-byte EEPROM sector

extern DeviceConfig cfg;

// Lifecycle
bool configLoad();         // returns false on first boot / corruption (loads defaults)
void configSave();
void configLoadDefaults(); // fills cfg from compile-time config.h constants

// Nav CSS shared between config_manager.cpp (HTML_HEAD) and main.cpp (HTML_PANEL_B).
// Compile-time string concatenation — zero runtime cost.
#define HTML_NAV_CSS \
    "nav{margin-bottom:14px;padding-bottom:8px;border-bottom:1px solid #ddd;font-size:.95em}" \
    "nav a{color:#0066cc;text-decoration:none}" \
    "nav span{font-weight:bold}"

#define HTML_FOOTER_CSS \
    "footer{margin-top:20px;padding-top:8px;border-top:1px solid #eee;" \
    "font-size:.8em;color:#999;text-align:center}"

// Fallback if the build script could not determine the version.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// __DATE__ expands at compile time — no runtime cost.
#define HTML_FOOTER_BODY \
    "<footer>v" FIRMWARE_VERSION " &middot; " __DATE__ "</footer>"

// Shared web helpers — usable by any ESP8266WebServer instance
void serveConfigPage(ESP8266WebServer &server, bool isPortal); // render and send the config form
void serveUpdatePage(ESP8266WebServer &server, bool isPortal); // render and send the firmware update form
void sendSavedPage(ESP8266WebServer &server);                  // send the "Saved! Restarting." page
void sendUpdateFailPage(ESP8266WebServer &server, const char *error); // send update error page
void processConfigSave(ESP8266WebServer &server); // read form args, update cfg, save to EEPROM

// Config portal
// Set to 1 to add DNSServer captive redirect (~+13 KB). Requires navigating to
// 192.168.4.1 manually when 0.
#define PORTAL_CAPTIVE_DNS 0
#define PORTAL_AP_PREFIX   "roomba-setup-"
#define PORTAL_TIMEOUT_MS  (5UL * 60UL * 1000UL) // 5 minutes
#ifndef PORTAL_AP_IP
#define PORTAL_AP_IP "192.168.4.1"
#endif

void portalStart();  // enter AP mode and start HTTP server
void portalHandle(); // call from loop() / setup() polling loop
bool portalActive(); // true while portal is running

// Optional callbacks set from main.cpp so the portal can render the home page
// and dispatch Roomba commands without depending on main.cpp internals.
typedef void (*PortalHomeHandler)(ESP8266WebServer &server);
typedef void (*PortalCmdHandler)(const String &action);
void portalSetHandlers(PortalHomeHandler home, PortalCmdHandler cmd);
