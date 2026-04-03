#include "secrets.h"

#define HOSTNAME "roomba_780" // e.g. roomba_780.local
#define BRC_PIN 14
#define ROOMBA_650_SLEEP_FIX 0
#define KEEP_ROOMBA_AWAKE 0

#define SET_ROOMBA_CLOCK 1
#define TIMEZONE TZ_Europe_Madrid
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

#define USE_SSL 0

#define ADC_VOLTAGE_DIVIDER 44.551316985
#define ENABLE_ADC_SLEEP 0

#define MAX_JSON_TOKENS 32   // max tokens for drive/command JSON payloads
#define MAX_SONG_TOKENS 130  // max tokens for song JSON array (up to 128 note values)
#define MAX_SONG_NOTES  128  // max note bytes in a song payload

#define RECONNECT_FREQ 5000
#define CONFIG_SEND_FREQ 120000
#define STATUS_REPORT_FREQ 10000
#define WAKEUP_FREQ 50000

// Config portal timing
#define WIFI_CONNECT_TIMEOUT_MS 15000UL   // ms to wait for WiFi before entering portal
#define WIFI_RECONNECT_PORTAL_MS 120000UL // ms of WiFi loss in loop() before entering portal

#define ROOMBA_MODEL "Roomba 780"
#define ROOMBA_FRIENDLY_NAME "Roomba 780"

#define PORTAL_AP_IP "192.168.4.1"

#define HOSTNAME_PREFIX "roomba-"

#define MQTT_SERVER "10.0.0.2"
#define MQTT_USER "homeassistant"
#define MQTT_PORT 1883

#define MQTT_DISCOVERY "homeassistant"
#define MQTT_DEVICE_CLASS "vacuum"
#define MQTT_DIVIDER "/"
#define MQTT_TOPIC_BASE MQTT_DISCOVERY MQTT_DIVIDER MQTT_DEVICE_CLASS MQTT_DIVIDER
#define MQTT_IDPREFIX "roomba_780_"
#define MQTT_COMMAND_TOPIC "command"
#define MQTT_STATE_TOPIC "state"
#define MQTT_CONFIG_TOPIC "config"

#define MQTT_DRIVE_TOPIC "drive"
#define MQTT_SONG_TOPIC "play_song"
#define MQTT_LWT_TOPIC "LWT"
#define MQTT_LWT_MESSAGE "offline"
