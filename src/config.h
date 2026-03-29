#include "secrets.h"

#define HOSTNAME "roomba" // e.g. roomba.local
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

#define RECONNECT_FREQ 5000
#define CONFIG_SEND_FREQ 120000
#define STATUS_REPORT_FREQ 10000
#define WAKEUP_FREQ 50000

#define ROOMBA_MODEL "Roomba 780"
#define ROOMBA_FRIENDLY_NAME "Roomba 780"

#define HOSTNAME_PREFIX "roomba-"

#define MQTT_SERVER "10.0.0.2"
#define MQTT_USER "homeassistant"
#define MQTT_PORT 1883

#define MQTT_DISCOVERY "homeassistant"
#define MQTT_DEVICE_CLASS "vacuum"
#define MQTT_DIVIDER "/"
#define MQTT_TOPIC_BASE MQTT_DISCOVERY MQTT_DIVIDER MQTT_DEVICE_CLASS MQTT_DIVIDER
#define MQTT_IDPREFIX "roomba_"
#define MQTT_COMMAND_TOPIC "command"
#define MQTT_STATE_TOPIC "state"
#define MQTT_CONFIG_TOPIC "config"

#define MQTT_DRIVE_TOPIC "drive"
#define MQTT_SONG_TOPIC "play_song"
#define MQTT_LWT_TOPIC "LWT"
#define MQTT_LWT_MESSAGE "offline"
