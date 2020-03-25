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

#define MQTT_CLIENT_ID "roomba.local"
#define MQTT_SERVER "10.0.0.2"
#define MQTT_USER "homeassistant"
#define MQTT_PORT 1883
#define MQTT_COMMAND_TOPIC "vacuum/command"
#define MQTT_STATE_TOPIC "vacuum/state"
#define MQTT_LWT_TOPIC "vacuum/LWT"
#define MQTT_LWT_MESSAGE "Online"
