#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <Roomba.h>
#include <PubSubClient.h>
#include <jsmn.h>
#include <TZ.h>
#include "config.h"
extern "C" {
#include "user_interface.h"
}

// Remote debugging over telnet. Just run:
// `telnet roomba.local` OR `nc roomba.local 23`
#if LOGGING
#include <RemoteDebug.h>
#define DLOG(msg, ...) if(Debug.isActive(Debug.DEBUG)){Debug.printf(msg, ##__VA_ARGS__);}
#define VLOG(msg, ...) if(Debug.isActive(Debug.VERBOSE)){Debug.printf(msg, ##__VA_ARGS__);}
RemoteDebug Debug;
#else
#define DLOG(msg, ...)
#define VLOG(msg, ...)
#endif

// Roomba setup
Roomba roomba(&Serial, Roomba::Baud115200);

// Roomba state
typedef struct {
  // Sensor values
  int16_t distance;
  uint8_t chargingState;
  uint16_t voltage;
  int16_t current;
  // Supposedly unsigned according to the OI docs, but I've seen it
  // underflow to ~65000mAh, so I think signed will work better.
  int16_t charge;
  uint16_t capacity;
  int16_t temp;
  uint8_t chargingSourcesAvailable;
  uint8_t OIMode;

  // Derived state
  bool cleaning;
  bool docked;
  bool returning;
  bool paused;

  int timestamp;
  bool sent;
} RoombaState;

RoombaState roombaState = {};

// Roomba sensor packet
uint8_t roombaPacket[150];
uint8_t sensors[] = {
  Roomba::SensorDistance, // PID 19, 2 bytes, mm, signed
  Roomba::SensorChargingState, // PID 21, 1 byte
  Roomba::SensorVoltage, // PID 22, 2 bytes, mV, unsigned
  Roomba::SensorCurrent, // PID 23, 2 bytes, mA, signed
  Roomba::SensorBatteryTemperature, // PID 24, 1 byte, signed
  Roomba::SensorBatteryCharge, // PID 25, 2 bytes, mAh, unsigned
  Roomba::SensorBatteryCapacity, // PID 26, 2 bytes, mAh, unsigned
  Roomba::SensorChargingSourcesAvailable, // PID 34, 1 byte, unsigned
  Roomba::SensorOIMode // PID 35, 1 byte, unsigned
};

// Network setup
#if USE_SSL
#include <cacert.h>
BearSSL::WiFiClientSecure wifiClient;
#else
WiFiClient wifiClient;
#endif
bool OTAStarted;

// MQTT setup
PubSubClient mqttClient(wifiClient);
const PROGMEM char *commandTopic = MQTT_COMMAND_TOPIC;
const PROGMEM char *statusTopic = MQTT_STATE_TOPIC;
const PROGMEM char *configTopic = MQTT_CONFIG_TOPIC;
const PROGMEM char *driveTopic = MQTT_DRIVE_TOPIC;
const PROGMEM char *songTopic = MQTT_SONG_TOPIC;
const PROGMEM char *lwtTopic = MQTT_LWT_TOPIC;
const PROGMEM char *lwtMessage = MQTT_LWT_MESSAGE;

// Timing state (declared here so mqttCallback can access lastStateMsgTime)
int lastStateMsgTime = 0;
int lastWakeupTime = 0;
int lastConnectTime = 0;
int lastConfigSend = 0;

// Forward declaration so mqttCallback can call sendStatus()
void sendStatus();

void wakeup() {
  DLOG("Wakeup Roomba\n");
  pinMode(BRC_PIN,OUTPUT);
  digitalWrite(BRC_PIN,LOW);
  delay(200);
  pinMode(BRC_PIN,INPUT);
  delay(200);
}

void wakeOnDock() {
  DLOG("Wakeup Roomba on dock\n");
  wakeup();
#if ROOMBA_650_SLEEP_FIX
  // Some black magic from @AndiTheBest to keep the Roomba awake on the dock
  // See https://github.com/johnboiles/esp-roomba-mqtt/issues/3#issuecomment-402096638
  delay(10);
  Serial.write(135); // Clean
  delay(150);
  Serial.write(143); // Dock
#endif
}

void turnOn() {
  DLOG("Turning on\n");
  wakeup();
  roomba.start();
  delay(50);
  roomba.safeMode();
  delay(50);
  roomba.cover();
  roombaState.cleaning = true;
  roombaState.returning = false;
  roombaState.paused = false;
}

void turnOff() {
  DLOG("Turning off\n");
  roomba.start();
  delay(50);
  roomba.power();
  roombaState.cleaning = false;
  roombaState.returning = false;
  roombaState.paused = false;
}

void stop() {
  if (roombaState.cleaning) {
    DLOG("Stopping\n");
    roomba.start();
    delay(50);
    roomba.cover();
    roombaState.cleaning = false;
    roombaState.returning = false;
    roombaState.paused = true;
  } else {
    DLOG("Not cleaning, can't stop\n");
  }
}

void cleanSpot() {
  DLOG("Cleaning Spot\n");
  wakeup();
  roomba.start();
  delay(50);
  roomba.safeMode();
  delay(50);
  roomba.spot();
  roombaState.cleaning = true;
  roombaState.returning = false;
  roombaState.paused = false;
}

void returnToBase() {
  DLOG("Returning to Base\n");
  wakeup();
  roomba.start();
  delay(50);
  roomba.safeMode();
  delay(50);
  roomba.dock();
  roombaState.cleaning = true;
  roombaState.returning = true;
  roombaState.paused = false;
}

void maxClean() {
  DLOG("Max Clean\n");
  wakeup();
  roomba.start();
  delay(50);
  roomba.safeMode();
  delay(50);
  roomba.maxClean();
  roombaState.cleaning = true;
  roombaState.returning = false;
}

void playSong(const uint8_t *notes, int len) {
  wakeup();
  roomba.start();
  delay(50);
  roomba.safeMode();
  delay(50);
  int chunkNum = ceil(len / 32.0);
  for (int i = 0; i < chunkNum; i++) {
    int first = i * 32 * sizeof(uint8_t);
    int length = i == chunkNum-1? len - i*32 : 32;
    length *= sizeof(uint8_t);
    uint8_t *chunk = (uint8_t*)malloc(len);
    memcpy(chunk, notes + first, length);
    int duration = 0;
    for (int j = 0; j < length; j++) {
        duration += (j + 1) % 2 == 0 ? chunk[j] : 0;
    }
    int ms = duration/64 * 1000 + 10;
    roomba.song(0, chunk, length);
    delay(50);
    roomba.playSong(0);
    free(chunk);
    delay(ms);
  }
}

void locate() {
  const uint8_t notes[] = {81,16,82,16,83,16,84,16,85,16,86,16,87,16,88,16,89,16,90,16,91,16,92,16};
  playSong(notes, sizeof(notes));
}

bool performCommand(const char *cmdchar) {
  // Char* string comparisons dont always work
  String cmd(cmdchar);

  // MQTT protocol commands
  if (cmd == "turn_on" || cmd == "clean" || cmd == "start") {
    turnOn();
  } else if (cmd == "turn_off") {
    turnOff();
  } else if (cmd == "stop" || cmd == "pause") {
    stop();
  } else if (cmd == "clean_spot") {
    cleanSpot();
  } else if (cmd == "locate") {
    locate();
  } else if (cmd == "max_clean") {
    maxClean();
  } else if (cmd == "return_to_base") {
    returnToBase();
  } else if (cmd == "wake_up") {
    wakeup();
  } else {
    return false;
  }
  return true;
}

void lowercase(char* str) {
  for(char* c=str; *c=tolower(*c);++c);
}

char* getMAC(const char* divider = "") {
  // build entity_id
  byte MAC[6];
  WiFi.macAddress(MAC);
  static char MACc[30];
  sprintf(MACc, "%02X%s%02X%s%02X%s%02X%s%02X%s%02X", MAC[0], divider, MAC[1], divider, MAC[2], divider, MAC[3], divider, MAC[4], divider, MAC[5]);
  lowercase(MACc);
  return MACc;
}

char* getEntityID(const char* prefix = MQTT_IDPREFIX) {
  static char entityID[100];
  sprintf(entityID, "%s%s", prefix, getMAC());
  lowercase(entityID);
  return entityID;
}

char* getMQTTTopic(const char* topic) {
  // build mqtt target topic
  static char mqttTopic[200];
  sprintf(mqttTopic, "%s%s%s%s", MQTT_TOPIC_BASE, getEntityID(), MQTT_DIVIDER, topic);
  return mqttTopic;
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
  if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
      strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
    return 0;
  }
  return -1;
}

bool driveRoomba(const char *commands) {
  jsmn_parser parser;
  jsmn_init(&parser);
  int jsonlen = jsmn_parse(&parser, commands, strlen(commands), NULL, 0);

  jsmn_init(&parser);
  jsmntok_t tokens[jsonlen];
  int r = jsmn_parse(&parser, commands, strlen(commands), tokens, sizeof(tokens)/sizeof(tokens[0]));
  if(r < 0) {
    printf("Failed to parse JSON: %d\n", r);
    return false;
  }
  if(r < 1 || tokens[0].type != JSMN_OBJECT) {
    printf("Object expected. Type: %d\n", tokens[0].type);
    return false;
  }

  int16_t velocity = 0, radius = 0;
  for(int i = 1; i < r; i++) {
    if(jsoneq(commands, &tokens[i], "velocity") == 0) {
        jsmntok_t *t = &tokens[i + 1];
        char vel[t->end - t->start];
        strncpy(vel, &commands[t->start], t->end - t->start);
        velocity = (int16_t)atoi(vel);
    } else if(jsoneq(commands, &tokens[i], "radius") == 0) {
        jsmntok_t *t = &tokens[i + 1];
        char rad[t->end - t->start];
        strncpy(rad, &commands[t->start], t->end - t->start);
        radius = (int16_t)atoi(rad);
    }
  }
  wakeup();
  roomba.start();
  delay(50);
  roomba.safeMode();
  delay(500);
  DLOG("Switched to safeMode\n");
  roomba.drive(velocity, radius);
  
  if(velocity == 0 && radius == 0) {
    delay(100);
    DLOG("Stop\n");
    roomba.start(); // switch to Passive mode
  }

  return true;
}

bool performPlaySong(const char *data) {
  jsmn_parser parser;
  jsmn_init(&parser);
  int jsonlen = jsmn_parse(&parser, data, strlen(data), NULL, 0);

  jsmn_init(&parser);
  jsmntok_t tokens[jsonlen];
  int r = jsmn_parse(&parser, data, strlen(data), tokens, sizeof(tokens)/sizeof(tokens[0]));
  if(r < 0) {
    DLOG("Failed to parse JSON: %d\n", r);
    return false;
  }
  if(r < 1 || tokens[0].type != JSMN_ARRAY) {
    DLOG("Array expected. Type: %d\n", tokens[0].type);
    return false;
  }

  int len = tokens[0].size;
  uint8_t notes[len];
  for(int i = 0; i < len; i++) {
    jsmntok_t *note = &tokens[i + 1];
    char notestr[note->end - note->start];
    strncpy(notestr, &data[note->start], note->end - note->start);
    notes[i] = (uint8_t)atoi(notestr);
  }
  playSong(notes, len);
  return true;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  DLOG("Received mqtt callback for topic %s\n", topic);
  if (strcmp(getMQTTTopic(commandTopic), topic) == 0) {
    // turn payload into a null terminated string
    char *cmd = (char *)malloc(length + 1);
    memcpy(cmd, payload, length);
    cmd[length] = 0;

    if(performCommand(cmd)) {
      sendStatus();                      // Immediate state feedback to HA
      lastStateMsgTime = millis();       // Reset timer to avoid duplicate publish
    } else {
      DLOG("Unknown command %s\n", cmd);
    }
    free(cmd);
  } else if (strcmp(getMQTTTopic(driveTopic), topic) == 0) {
    // turn payload into a null terminated string
    char *cmd = (char *)malloc(length + 1);
    memcpy(cmd, payload, length);
    cmd[length] = 0;

    if(!driveRoomba(cmd)) {
      DLOG("Invalid drive commands: %s\n", cmd);
    }
    free(cmd);
  } else if (strcmp(getMQTTTopic(songTopic), topic) == 0) {
    // turn payload into a null terminated string
    char *cmd = (char *)malloc(length + 1);
    memcpy(cmd, payload, length);
    cmd[length] = 0;

    if(!performPlaySong(cmd)) {
      DLOG("Invalid notes: %s\n", cmd);
    }
    free(cmd);
  }
}

float readADC(int samples) {
  // Basic code to read from the ADC
  int adc = 0;
  for (int i = 0; i < samples; i++) {
    delay(1);
    adc += analogRead(A0);
  }
  adc = adc / samples;
  float mV = adc * ADC_VOLTAGE_DIVIDER;
  VLOG("ADC for %d is %.1fmV with %d samples\n", adc, mV, samples);
  return mV;
}

void setDateTime() {
  configTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    now = time(nullptr);
  }
  struct tm * timeinfo;
  time(&now);
  timeinfo = localtime(&now);
  #ifdef SET_ROOMBA_CLOCK
  wakeup();
  roomba.start();
  roomba.setDayTime(timeinfo->tm_wday, timeinfo->tm_hour, timeinfo->tm_min);
  #endif
}

void debugCallback() {
  #if LOGGING
  String cmd = Debug.getLastCommand();

  // Debugging commands via telnet
  if (performCommand(cmd.c_str())) {
  } else if (cmd == "quit") {
    DLOG("Stopping Roomba\n");
    Serial.write(173);
  } else if (cmd == "rreset") {
    DLOG("Resetting Roomba\n");
    roomba.reset();
  } else if (cmd == "mqtthello") {
    mqttClient.publish("vacuum/hello", "hello there");
  } else if (cmd == "version") {
    const char compile_date[] = __DATE__ " " __TIME__;
    DLOG("Compiled on: %s\n", compile_date);
  } else if (cmd == "baud115200") {
    DLOG("Setting baud to 115200\n");
    Serial.begin(115200);
    delay(100);
  } else if (cmd == "baud19200") {
    DLOG("Setting baud to 19200\n");
    Serial.begin(19200);
    delay(100);
  } else if (cmd == "baud57600") {
    DLOG("Setting baud to 57600\n");
    Serial.begin(57600);
    delay(100);
  } else if (cmd == "baud38400") {
    DLOG("Setting baud to 38400\n");
    Serial.begin(38400);
    delay(100);
  } else if (cmd == "sleep5") {
    DLOG("Going to sleep for 5 seconds\n");
    delay(100);
    ESP.deepSleep(5e6);
  } else if (cmd == "wake") {
    DLOG("Toggle BRC pin\n");
    wakeup();
  } else if (cmd == "readadc") {
    float adc = readADC(10);
    DLOG("ADC voltage is %.1fmV\n", adc);
  } else if (cmd == "streamresume") {
    DLOG("Resume streaming\n");
    roomba.streamCommand(Roomba::StreamCommandResume);
  } else if (cmd == "streampause") {
    DLOG("Pause streaming\n");
    roomba.streamCommand(Roomba::StreamCommandPause);
  } else if (cmd == "stream") {
    DLOG("Requesting stream\n");
    roomba.stream(sensors, sizeof(sensors));
  } else if (cmd == "streamreset") {
    DLOG("Resetting stream\n");
    roomba.stream({}, 0);
  } else if (cmd == "time") {
    setDateTime();
  } else {
    DLOG("Unknown command %s\n", cmd.c_str());
  }
  #endif
}

void sleepIfNecessary() {
#if ENABLE_ADC_SLEEP
  // Check the battery, if it's too low, sleep the ESP (so we don't murder the battery)
  float mV = readADC(10);
  // According to this post, you want to stop using NiMH batteries at about 0.9V per cell
  // https://electronics.stackexchange.com/a/35879 For a 12 cell battery like is in the Roomba,
  // That's 10.8 volts.
  if (mV < 10800) {
    // Fire off a quick message with our most recent state, if MQTT is connected
    DLOG("Battery voltage is low (%.1fV). Sleeping for 10 minutes\n", mV / 1000);
    if (mqttClient.connected()) {
      String json = "{";
      json += "\"battery_level\":\" 0,";
      json += "\"cleaning\":\" false";
      json += "\"docked\":\" false";
      json += "\"charging\":\" false";
      json += "\"voltage\":\"";
      json += String(mV / 1000);
      json += "\"charge\":\" 0";
      json += "}";
      mqttClient.publish(getMQTTTopic(statusTopic), json.c_str(), true);
    }
    delay(200);

    // Sleep for 10 minutes
    ESP.deepSleep(600e6);
  }
#endif
}

bool parseRoombaStateFromStreamPacket(uint8_t *packet, int length, RoombaState *state) {
  state->timestamp = millis();
  int i = 0;
  while (i < length) {
    switch(packet[i]) {
      case Roomba::Sensors7to26: // 0
        i += 27;
        break;
      case Roomba::Sensors7to16: // 1
        i += 11;
        break;
      case Roomba::SensorVirtualWall: // 13
        i += 2;
        break;
      case Roomba::SensorDistance: // 19
        state->distance = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorChargingState: // 21
        state->chargingState = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorVoltage: // 22
        state->voltage = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorCurrent: // 23
        state->current = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryTemperature: //24
        state->temp = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorBatteryCharge: // 25
        state->charge = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryCapacity: //26
        state->capacity = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorChargingSourcesAvailable: //34
        state->chargingSourcesAvailable = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorOIMode: //35
        state->OIMode = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorBumpsAndWheelDrops: // 7
        i += 2;
        break;
      case 128: // Unknown
        i += 2;
        break;
      default:
        VLOG("Unhandled Packet ID %d\n", packet[i]);
        return false;
        break;
    }
  }
  return true;
}

void verboseLogPacket(uint8_t *packet, uint8_t length) {
    VLOG("Packet: ");
    for (int i = 0; i < length; i++) {
      VLOG("%d ", packet[i]);
    }
    VLOG("\n");
}

void readSensorPacket() {
  uint8_t packetLength;
  bool received = roomba.pollSensors(roombaPacket, sizeof(roombaPacket), &packetLength);
  if (received) {
    RoombaState rs = {};
    bool parsed = parseRoombaStateFromStreamPacket(roombaPacket, packetLength, &rs);
    verboseLogPacket(roombaPacket, packetLength);
    if (parsed && rs.temp != 0) {
      rs.cleaning = roombaState.cleaning;
      rs.docked = roombaState.docked;
      rs.returning = roombaState.returning;
      roombaState = rs;
      VLOG("Got Packet of len=%d! OIMode:%d Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", packetLength, roombaState.OIMode, roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
      roombaState.cleaning = false;
      roombaState.docked = false;
      if (roombaState.current < -400) {
        roombaState.cleaning = true;
      } else if (roombaState.current > -50) {
        roombaState.docked = true;
        roombaState.returning = false;
      }
    } else {
      VLOG("Failed to parse packet, packetLength:%d, Temperature:%d\n", packetLength, rs.temp);
    }
  }
}

void onOTAStart() {
  DLOG("Starting OTA session\n");
  DLOG("Pause streaming\n");
  roomba.streamCommand(Roomba::StreamCommandPause);
  OTAStarted = true;
}

void setup() {
  // High-impedence on the BRC_PIN
  pinMode(BRC_PIN,INPUT);

  // Sleep immediately if ENABLE_ADC_SLEEP and the battery is low
  sleepIfNecessary();

  // Set Hostname.
  String hostname(HOSTNAME);
  WiFi.persistent(false);       // Don't write credentials to flash on every connect
  WiFi.setAutoReconnect(true);  // SDK-level auto-reconnect on WiFi drop
  WiFi.setSleepMode(WIFI_MODEM_SLEEP); // Modem sleep between beacons (~20-30% WiFi power saving)
  WiFi.hostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.begin();
  ArduinoOTA.onStart(onOTAStart);

  // Synchronize time using SNTP. This is necessary to verify that
  // the TLS certificates offered by the server are currently valid.
  setDateTime();
  #if USE_SSL
  wifiClient.setCACert_P(caCert, caCertLen);
  #endif

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setKeepAlive(60);
  mqttClient.setCallback(mqttCallback);

  #if LOGGING
  Debug.begin((const char *)hostname.c_str());
  Debug.setResetCmdEnabled(true);
  Debug.setCallBackProjectCmds(debugCallback);
  Debug.setSerialEnabled(false);
  #endif

  roomba.start();
  delay(100);

  // Reset stream sensor values
  roomba.stream({}, 0);
  delay(100);

  // Request sensor stream
  roomba.stream(sensors, sizeof(sensors));
}

void reconnect() {
  DLOG("Attempting MQTT connection...\n");
  // Attempt to connect. LWT retain=true so broker keeps "offline" for HA restarts.
  if (mqttClient.connect(getEntityID(HOSTNAME_PREFIX), MQTT_USER, MQTT_PASSWORD, getMQTTTopic(lwtTopic), 0, true, lwtMessage)) {
    DLOG("MQTT connected\n");
    // Publish "online" so HA marks the entity as available
    mqttClient.publish(getMQTTTopic(lwtTopic), "online", true);
    mqttClient.subscribe(getMQTTTopic(commandTopic));
    mqttClient.subscribe(getMQTTTopic(driveTopic));
    mqttClient.subscribe(getMQTTTopic(songTopic));
  } else {
    DLOG("MQTT failed rc=%d try again in 5 seconds\n", mqttClient.state());
    #if USE_SSL
    char buf[256];
    int ernum = wifiClient.getLastSSLError(buf, 256);
    DLOG("MQTT SSL Error: %d - %s\n", ernum, buf);
    #endif
  }
}

char* getSensorDiscoveryTopic(const char* component, const char* entitySuffix) {
  static char sensorTopic[200];
  sprintf(sensorTopic, "%s/%s/%s%s/config",
    MQTT_DISCOVERY, component, getEntityID(), entitySuffix);
  return sensorTopic;
}

void sendConfig() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending config\n");
    return;
  }
  char baseTopic[200];
  sprintf(baseTopic, "%s%s", MQTT_TOPIC_BASE, getEntityID());
  String json = F("{");
  json += F("\"name\":\""); json += ROOMBA_FRIENDLY_NAME; json += F("\",");
  json += F("\"unique_id\":\""); json += getEntityID(); json += F("\",");
  json += F("\"~\":\""); json += baseTopic; json += F("\",");
  json += F("\"stat_t\":\"~/state\",");
  json += F("\"cmd_t\":\"~/command\",");
  json += F("\"send_cmd_t\":\"~/command\",");
  json += F("\"json_attr_t\":\"~/state\",");
  json += F("\"avty_t\":\"~/LWT\",");
  json += F("\"pl_avail\":\"online\",");
  json += F("\"pl_not_avail\":\"offline\",");
  json += F("\"sup_feat\":[\"start\",\"stop\",\"pause\",\"return_home\",\"locate\",\"clean_spot\",\"send_command\"],");
  json += F("\"dev\":{");
  json += F("\"name\":\""); json += ROOMBA_MODEL; json += F("\",");
  json += F("\"ids\":[\""); json += getEntityID(); json += F("\"],");
  json += F("\"mf\":\"iRobot\",");
  json += F("\"mdl\":\""); json += ROOMBA_MODEL; json += F("\"");
  json += F("}}");
  DLOG("Reporting vacuum config: %s\n", json.c_str());
  mqttClient.publish(getMQTTTopic(configTopic), json.c_str(), true);
}

void sendBatterySensorConfig() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending battery sensor config\n");
    return;
  }
  String statTopic = String(getMQTTTopic(statusTopic));
  String avtyTopic = String(getMQTTTopic(lwtTopic));
  String json = F("{");
  json += F("\"name\":\""); json += ROOMBA_FRIENDLY_NAME; json += F(" Battery\",");
  json += F("\"unique_id\":\""); json += getEntityID(); json += F("_battery\",");
  json += F("\"device_class\":\"battery\",");
  json += F("\"state_class\":\"measurement\",");
  json += F("\"unit_of_measurement\":\"%\",");
  json += F("\"stat_t\":\""); json += statTopic; json += F("\",");
  json += F("\"val_tpl\":\"{{value_json.battery_level}}\",");
  json += F("\"avty_t\":\""); json += avtyTopic; json += F("\",");
  json += F("\"pl_avail\":\"online\",");
  json += F("\"pl_not_avail\":\"offline\",");
  json += F("\"dev\":{\"ids\":[\""); json += getEntityID(); json += F("\"]}");
  json += F("}");
  DLOG("Reporting battery sensor config: %s\n", json.c_str());
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_battery"), json.c_str(), true);
}

void sendChargingSensorConfig() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending charging sensor config\n");
    return;
  }
  String statTopic = String(getMQTTTopic(statusTopic));
  String avtyTopic = String(getMQTTTopic(lwtTopic));
  String json = F("{");
  json += F("\"name\":\""); json += ROOMBA_FRIENDLY_NAME; json += F(" Charging\",");
  json += F("\"unique_id\":\""); json += getEntityID(); json += F("_charging\",");
  json += F("\"device_class\":\"battery_charging\",");
  json += F("\"stat_t\":\""); json += statTopic; json += F("\",");
  json += F("\"val_tpl\":\"{{value_json.charging | lower}}\",");
  json += F("\"pl_on\":\"true\",");
  json += F("\"pl_off\":\"false\",");
  json += F("\"avty_t\":\""); json += avtyTopic; json += F("\",");
  json += F("\"pl_avail\":\"online\",");
  json += F("\"pl_not_avail\":\"offline\",");
  json += F("\"dev\":{\"ids\":[\""); json += getEntityID(); json += F("\"]}");
  json += F("}");
  DLOG("Reporting charging sensor config: %s\n", json.c_str());
  mqttClient.publish(getSensorDiscoveryTopic("binary_sensor", "_charging"), json.c_str(), true);
}

String getCurrentState() {
  String curState = "idle";
  if (roombaState.docked)
    curState = "docked";
  else if (roombaState.returning)
    curState = "returning";
  else if (roombaState.cleaning)
    curState = "cleaning";
  else if (roombaState.paused)
    curState = "paused";
  return curState;
}

String boolToString(bool value) {
  return value? "true" : "false";
}

void sendStatus() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending status\n");
    return;
  }
  DLOG("Reporting packet Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
  int16_t batteryLevel = (roombaState.capacity) ? (roombaState.charge * 100)/roombaState.capacity : 0;
  boolean docked = roombaState.chargingSourcesAvailable == Roomba::ChargeAvailableDock;
  boolean isCharging = roombaState.chargingState == Roomba::ChargeStateReconditioningCharging
    || roombaState.chargingState == Roomba::ChargeStateFullCharging
    || roombaState.chargingState == Roomba::ChargeStateTrickleCharging;
  String json = F("{");
  json += F("\"battery_level\":"); json += batteryLevel;
  json += F(",\"cleaning\":"); json += boolToString(roombaState.cleaning);
  json += F(",\"returning\":"); json += boolToString(roombaState.returning);
  json += F(",\"docked\":"); json += boolToString(docked);
  json += F(",\"charging\":"); json += boolToString(isCharging);
  json += F(",\"chargingState\":"); json += roombaState.chargingState;
  json += F(",\"voltage\":"); json += roombaState.voltage;
  json += F(",\"current\":"); json += roombaState.current;
  json += F(",\"charge\":"); json += roombaState.charge;
  json += F(",\"capacity\":"); json += roombaState.capacity;
  json += F(",\"distance\":"); json += roombaState.distance;
  json += F(",\"batteryTemperature\":"); json += roombaState.temp;
  json += F(",\"chargingSourcesAvailable\":"); json += roombaState.chargingSourcesAvailable;
  json += F(",\"OIMode\":"); json += roombaState.OIMode;
  json += F(",\"state\":\""); json += getCurrentState(); json += F("\"");
  json += F(",\"ssid\":\""); json += WiFi.SSID(); json += F("\"");
  json += F(",\"rssi\":"); json += WiFi.RSSI();
  json += F(",\"ip\":\""); json += WiFi.localIP().toString(); json += F("\"");
  json += F(",\"hostname\":\""); json += WiFi.getHostname(); json += F("\"");
  json += F("}");
  mqttClient.publish(getMQTTTopic(statusTopic), json.c_str());
}

void loop() {
  // Important callbacks that _must_ happen every cycle
  ArduinoOTA.handle();
  yield();
  #if LOGGING
  Debug.handle();
  #endif

  // Skip all other logic if we're running an OTA update
  if (OTAStarted) {
    return;
  }

  long now = millis();
  // WiFi watchdog: if WiFi dropped, attempt reconnect and skip MQTT logic
  if (WiFi.status() != WL_CONNECTED) {
    DLOG("WiFi disconnected, reconnecting...\n");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    return;
  }
  // If MQTT client can't connect to broker, then reconnect
  if (!mqttClient.connected() && (now - lastConnectTime) > RECONNECT_FREQ) {
    DLOG("Reconnecting MQTT\n");
    lastConnectTime = now;
    lastConfigSend = now;
    reconnect();
    sendConfig();
    sendBatterySensorConfig();
    sendChargingSensorConfig();
  } else if((now - lastConfigSend) > CONFIG_SEND_FREQ) {
    lastConfigSend = now;
    sendConfig();
    sendBatterySensorConfig();
    sendChargingSensorConfig();
  }

  #if KEEP_ROOMBA_AWAKE
  // Wakeup the roomba at fixed intervals
  if (now - lastWakeupTime > WAKEUP_FREQ) {
    lastWakeupTime = now;
    if (!roombaState.cleaning) {
      if (roombaState.docked) {
        wakeOnDock();
      } else {
        wakeup();
      }
    } else {
      wakeup();
    }
  }
  #endif

  // Report the status over mqtt at fixed intervals
  if (now - lastStateMsgTime > STATUS_REPORT_FREQ) {
    lastStateMsgTime = now;
    if (now - roombaState.timestamp > 30000 || roombaState.sent) {
      DLOG("Roomba state already sent (%.1fs old)\n", (now - roombaState.timestamp)/1000.0);
      DLOG("Request stream\n");
      roomba.stream(sensors, sizeof(sensors));
    } else {
      sendStatus();
      roombaState.sent = true;
    }
    sleepIfNecessary();
  }

  readSensorPacket();
  mqttClient.loop();
}
