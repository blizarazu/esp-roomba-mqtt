#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <Updater.h>
#include <Roomba.h>
#include <PubSubClient.h>
#include <jsmn.h>
#include <TZ.h>
#include "config.h"
#include "config_manager.h"
extern "C"
{
#include "user_interface.h"
}

// Remote debugging over telnet. Just run:
// `telnet roomba.local` OR `nc roomba.local 23`
#if LOGGING
#include <RemoteDebug.h>
#define DLOG(msg, ...)                \
  if (Debug.isActive(Debug.DEBUG))    \
  {                                   \
    Debug.printf(msg, ##__VA_ARGS__); \
  }
#define VLOG(msg, ...)                \
  if (Debug.isActive(Debug.VERBOSE))  \
  {                                   \
    Debug.printf(msg, ##__VA_ARGS__); \
  }
RemoteDebug Debug;
#else
#define DLOG(msg, ...)
#define VLOG(msg, ...)
#endif

// Roomba setup
Roomba roomba(&Serial, Roomba::Baud115200);

// Roomba state
typedef struct
{
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
bool roombaConnected = false;

// Roomba sensor packet
uint8_t roombaPacket[150];
uint8_t sensors[] = {
    Roomba::SensorDistance,                 // PID 19, 2 bytes, mm, signed
    Roomba::SensorChargingState,            // PID 21, 1 byte
    Roomba::SensorVoltage,                  // PID 22, 2 bytes, mV, unsigned
    Roomba::SensorCurrent,                  // PID 23, 2 bytes, mA, signed
    Roomba::SensorBatteryTemperature,       // PID 24, 1 byte, signed
    Roomba::SensorBatteryCharge,            // PID 25, 2 bytes, mAh, unsigned
    Roomba::SensorBatteryCapacity,          // PID 26, 2 bytes, mAh, unsigned
    Roomba::SensorChargingSourcesAvailable, // PID 34, 1 byte, unsigned
    Roomba::SensorOIMode                    // PID 35, 1 byte, unsigned
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

// Shared buffer for JSON building — all JSON-emitting functions are single-threaded
// and non-reentrant, so one buffer is enough and avoids heap fragmentation.
static char jsonBuf[768];

static const char *boolStr(bool v) { return v ? "true" : "false"; }

// Forward declarations
void sendStatus();
String getCurrentState();
static void roombaDeepWakeAndReconnect();

// Control web server (always-on in normal WiFi mode, port 80)
ESP8266WebServer controlServer(80);

void wakeup()
{
  DLOG("Wakeup Roomba\n");
  pinMode(BRC_PIN, OUTPUT);
  digitalWrite(BRC_PIN, LOW);
  delay(200);          // short pulse for keep-alive; safely below baud-rate-change threshold
  pinMode(BRC_PIN, INPUT);
  delay(200);
}

static void roombaWakeAndSafe()
{
  if (!roombaConnected)
    roombaDeepWakeAndReconnect();
  else
    wakeup();
  roomba.start();
  delay(50);
  roomba.safeMode();
  delay(50);
}

void wakeOnDock()
{
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

// Initialise (or re-initialise) the Roomba OI and sensor stream.
// Called at boot and again after waking from deep sleep, which resets the
// Roomba's OI back to OFF mode and discards the previous stream subscription.
static void initRoombaStream()
{
  DLOG("Initialising Roomba OI and sensor stream\n");
  roomba.start();
  delay(100);
  roomba.stream({}, 0);    // cancel any stale stream subscription
  delay(50);
  roomba.stream(sensors, sizeof(sensors));
}

// Aggressive wake from deep sleep (green light off, roombaConnected == false).
// Sends 3 BRC pulses then fully re-initialises the OI and sensor stream.
static void roombaDeepWakeAndReconnect()
{
  DLOG("Roomba deep sleep — aggressive wake\n");
  // 3 × 500ms BRC pulses. When OI is OFF (deep sleep), BRC is a wake signal,
  // not a baud rate change, so 500ms is safe here (unlike the keep-alive pulse).
  for (int i = 0; i < 3; i++)
  {
    pinMode(BRC_PIN, OUTPUT);
    digitalWrite(BRC_PIN, LOW);
    delay(500);
    pinMode(BRC_PIN, INPUT);
    delay(200);
  }
  delay(300);           // allow OI to boot before sending commands
  initRoombaStream();
  delay(100);
}

void turnOn()
{
  DLOG("Turning on\n");
  roombaWakeAndSafe();
  roomba.cover();
  roombaState.cleaning = true;
  roombaState.returning = false;
  roombaState.paused = false;
}

void turnOff()
{
  DLOG("Turning off\n");
  roomba.start();
  delay(50);
  roomba.power();
  roombaState.cleaning = false;
  roombaState.returning = false;
  roombaState.paused = false;
}

void stop()
{
  if (roombaState.cleaning)
  {
    DLOG("Stopping\n");
    roomba.start();
    delay(50);
    roomba.cover();
    roombaState.cleaning = false;
    roombaState.returning = false;
    roombaState.paused = true;
  }
  else
  {
    DLOG("Not cleaning, can't stop\n");
  }
}

void cleanSpot()
{
  DLOG("Cleaning Spot\n");
  roombaWakeAndSafe();
  roomba.spot();
  roombaState.cleaning = true;
  roombaState.returning = false;
  roombaState.paused = false;
}

void returnToBase()
{
  DLOG("Returning to Base\n");
  roombaWakeAndSafe();
  roomba.dock();
  roombaState.cleaning = true;
  roombaState.returning = true;
  roombaState.paused = false;
}

void maxClean()
{
  DLOG("Max Clean\n");
  roombaWakeAndSafe();
  roomba.maxClean();
  roombaState.cleaning = true;
  roombaState.returning = false;
  roombaState.paused = false;
}

void playSong(const uint8_t *notes, int len)
{
  roombaWakeAndSafe();
  int chunkNum = ceil(len / 32.0);
  for (int i = 0; i < chunkNum; i++)
  {
    int first = i * 32 * sizeof(uint8_t);
    int length = i == chunkNum - 1 ? len - i * 32 : 32;
    length *= sizeof(uint8_t);
    uint8_t *chunk = (uint8_t *)malloc(len);
    memcpy(chunk, notes + first, length);
    int duration = 0;
    for (int j = 0; j < length; j++)
    {
      duration += (j + 1) % 2 == 0 ? chunk[j] : 0;
    }
    int ms = duration / 64 * 1000 + 10;
    roomba.song(0, chunk, length);
    delay(50);
    roomba.playSong(0);
    free(chunk);
    delay(ms);
  }
}

void locate()
{
  const uint8_t notes[] = {81, 16, 82, 16, 83, 16, 84, 16, 85, 16, 86, 16, 87, 16, 88, 16, 89, 16, 90, 16, 91, 16, 92, 16};
  playSong(notes, sizeof(notes));
}

bool performCommand(const char *cmdchar)
{
  // Char* string comparisons dont always work
  String cmd(cmdchar);

  // MQTT protocol commands
  if (cmd == "turn_on" || cmd == "clean" || cmd == "start")
  {
    turnOn();
  }
  else if (cmd == "turn_off")
  {
    turnOff();
  }
  else if (cmd == "stop" || cmd == "pause")
  {
    stop();
  }
  else if (cmd == "clean_spot")
  {
    cleanSpot();
  }
  else if (cmd == "locate")
  {
    locate();
  }
  else if (cmd == "max_clean")
  {
    maxClean();
  }
  else if (cmd == "return_to_base")
  {
    returnToBase();
  }
  else if (cmd == "wake_up")
  {
    if (!roombaConnected)
      roombaDeepWakeAndReconnect();
    else
      wakeup();
  }
  else
  {
    return false;
  }
  return true;
}

void lowercase(char *str)
{
  for (char *c = str; *c = tolower(*c); ++c)
    ;
}

char *getMAC(const char *divider = "")
{
  // build entity_id
  byte MAC[6];
  WiFi.macAddress(MAC);
  static char MACc[30];
  sprintf(MACc, "%02X%s%02X%s%02X%s%02X%s%02X%s%02X", MAC[0], divider, MAC[1], divider, MAC[2], divider, MAC[3], divider, MAC[4], divider, MAC[5]);
  lowercase(MACc);
  return MACc;
}

char *getEntityID(const char *prefix = MQTT_IDPREFIX)
{
  static char entityID[100];
  sprintf(entityID, "%s%s", prefix, getMAC());
  lowercase(entityID);
  return entityID;
}

char *getMQTTTopic(const char *topic)
{
  // build mqtt target topic
  static char mqttTopic[200];
  sprintf(mqttTopic, "%s%s%s%s", MQTT_TOPIC_BASE, getEntityID(), MQTT_DIVIDER, topic);
  return mqttTopic;
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
  if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
      strncmp(json + tok->start, s, tok->end - tok->start) == 0)
  {
    return 0;
  }
  return -1;
}

bool driveRoomba(const char *commands)
{
  jsmntok_t tokens[MAX_JSON_TOKENS];
  jsmn_parser parser;
  jsmn_init(&parser);
  int r = jsmn_parse(&parser, commands, strlen(commands), tokens, MAX_JSON_TOKENS);
  if (r < 0)
  {
    DLOG("Failed to parse JSON: %d\n", r);
    return false;
  }
  if (r < 1 || tokens[0].type != JSMN_OBJECT)
  {
    DLOG("Object expected. Type: %d\n", tokens[0].type);
    return false;
  }

  int16_t velocity = 0, radius = 0;
  for (int i = 1; i < r; i++)
  {
    if (jsoneq(commands, &tokens[i], "velocity") == 0)
    {
      jsmntok_t *t = &tokens[i + 1];
      char val[12];
      int vlen = min((int)(t->end - t->start), (int)(sizeof(val) - 1));
      strncpy(val, &commands[t->start], vlen);
      val[vlen] = '\0';
      velocity = (int16_t)atoi(val);
    }
    else if (jsoneq(commands, &tokens[i], "radius") == 0)
    {
      jsmntok_t *t = &tokens[i + 1];
      char val[12];
      int vlen = min((int)(t->end - t->start), (int)(sizeof(val) - 1));
      strncpy(val, &commands[t->start], vlen);
      val[vlen] = '\0';
      radius = (int16_t)atoi(val);
    }
  }
  roombaWakeAndSafe();
  delay(450); // extra settling time for drive mode (total 500 ms after safeMode)
  DLOG("Switched to safeMode\n");
  roomba.drive(velocity, radius);

  if (velocity == 0 && radius == 0)
  {
    delay(100);
    DLOG("Stop\n");
    roomba.start(); // switch to Passive mode
  }

  return true;
}

bool performPlaySong(const char *data)
{
  jsmntok_t tokens[MAX_SONG_TOKENS];
  jsmn_parser parser;
  jsmn_init(&parser);
  int r = jsmn_parse(&parser, data, strlen(data), tokens, MAX_SONG_TOKENS);
  if (r < 0)
  {
    DLOG("Failed to parse JSON: %d\n", r);
    return false;
  }
  if (r < 1 || tokens[0].type != JSMN_ARRAY)
  {
    DLOG("Array expected. Type: %d\n", tokens[0].type);
    return false;
  }

  int len = min((int)tokens[0].size, MAX_SONG_NOTES);
  uint8_t notes[MAX_SONG_NOTES];
  for (int i = 0; i < len; i++)
  {
    jsmntok_t *note = &tokens[i + 1];
    char notestr[8];
    int nlen = min((int)(note->end - note->start), (int)(sizeof(notestr) - 1));
    strncpy(notestr, &data[note->start], nlen);
    notestr[nlen] = '\0';
    notes[i] = (uint8_t)atoi(notestr);
  }
  playSong(notes, len);
  return true;
}

static char *payloadToStr(byte *payload, unsigned int length)
{
  char *s = (char *)malloc(length + 1);
  if (s)
  {
    memcpy(s, payload, length);
    s[length] = '\0';
  }
  return s;
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  DLOG("Received mqtt callback for topic %s\n", topic);
  if (strcmp(getMQTTTopic(commandTopic), topic) == 0)
  {
    char *cmd = payloadToStr(payload, length);
    if (cmd)
    {
      if (performCommand(cmd))
      {
        sendStatus();                // Immediate state feedback to HA
        lastStateMsgTime = millis(); // Reset timer to avoid duplicate publish
      }
      else
      {
        DLOG("Unknown command %s\n", cmd);
      }
      free(cmd);
    }
  }
  else if (strcmp(getMQTTTopic(driveTopic), topic) == 0)
  {
    char *cmd = payloadToStr(payload, length);
    if (cmd)
    {
      if (!driveRoomba(cmd))
        DLOG("Invalid drive commands: %s\n", cmd);
      free(cmd);
    }
  }
  else if (strcmp(getMQTTTopic(songTopic), topic) == 0)
  {
    char *cmd = payloadToStr(payload, length);
    if (cmd)
    {
      if (!performPlaySong(cmd))
        DLOG("Invalid notes: %s\n", cmd);
      free(cmd);
    }
  }
}

float readADC(int samples)
{
  // Basic code to read from the ADC
  int adc = 0;
  for (int i = 0; i < samples; i++)
  {
    delay(1);
    adc += analogRead(A0);
  }
  adc = adc / samples;
  float mV = adc * ADC_VOLTAGE_DIVIDER;
  VLOG("ADC for %d is %.1fmV with %d samples\n", adc, mV, samples);
  return mV;
}

void setDateTime()
{
  configTime(TIMEZONE, cfg.ntp_server1, cfg.ntp_server2);
  time_t now = time(nullptr);
  uint32_t ntpStart = millis();
  while (now < 8 * 3600 * 2 && millis() - ntpStart < 30000)
  {
    delay(500);
    now = time(nullptr);
  }
  struct tm *timeinfo;
  time(&now);
  timeinfo = localtime(&now);
#if SET_ROOMBA_CLOCK
  wakeup();
  roomba.start();
  roomba.setDayTime(timeinfo->tm_wday, timeinfo->tm_hour, timeinfo->tm_min);
#endif
}

void debugCallback()
{
#if LOGGING
  String cmd = Debug.getLastCommand();

  // Debugging commands via telnet
  if (performCommand(cmd.c_str()))
  {
  }
  else if (cmd == "quit")
  {
    DLOG("Stopping Roomba\n");
    Serial.write(173);
  }
  else if (cmd == "rreset")
  {
    DLOG("Resetting Roomba\n");
    roomba.reset();
  }
  else if (cmd == "mqtthello")
  {
    mqttClient.publish("vacuum/hello", "hello there");
  }
  else if (cmd == "version")
  {
    const char compile_date[] = __DATE__ " " __TIME__;
    DLOG("Compiled on: %s\n", compile_date);
  }
  else if (cmd == "baud115200")
  {
    DLOG("Setting baud to 115200\n");
    Serial.begin(115200);
    delay(100);
  }
  else if (cmd == "baud19200")
  {
    DLOG("Setting baud to 19200\n");
    Serial.begin(19200);
    delay(100);
  }
  else if (cmd == "baud57600")
  {
    DLOG("Setting baud to 57600\n");
    Serial.begin(57600);
    delay(100);
  }
  else if (cmd == "baud38400")
  {
    DLOG("Setting baud to 38400\n");
    Serial.begin(38400);
    delay(100);
  }
  else if (cmd == "sleep5")
  {
    DLOG("Going to sleep for 5 seconds\n");
    delay(100);
    ESP.deepSleep(5e6);
  }
  else if (cmd == "wake")
  {
    DLOG("Toggle BRC pin\n");
    wakeup();
  }
  else if (cmd == "readadc")
  {
    float adc = readADC(10);
    DLOG("ADC voltage is %.1fmV\n", adc);
  }
  else if (cmd == "streamresume")
  {
    DLOG("Resume streaming\n");
    roomba.streamCommand(Roomba::StreamCommandResume);
  }
  else if (cmd == "streampause")
  {
    DLOG("Pause streaming\n");
    roomba.streamCommand(Roomba::StreamCommandPause);
  }
  else if (cmd == "stream")
  {
    DLOG("Requesting stream\n");
    roomba.stream(sensors, sizeof(sensors));
  }
  else if (cmd == "streamreset")
  {
    DLOG("Resetting stream\n");
    roomba.stream({}, 0);
  }
  else if (cmd == "time")
  {
    setDateTime();
  }
  else
  {
    DLOG("Unknown command %s\n", cmd.c_str());
  }
#endif
}

void sleepIfNecessary()
{
#if ENABLE_ADC_SLEEP
  // Check the battery, if it's too low, sleep the ESP (so we don't murder the battery)
  float mV = readADC(10);
  // According to this post, you want to stop using NiMH batteries at about 0.9V per cell
  // https://electronics.stackexchange.com/a/35879 For a 12 cell battery like is in the Roomba,
  // That's 10.8 volts.
  if (mV < 10800)
  {
    // Fire off a quick message with our most recent state, if MQTT is connected
    DLOG("Battery voltage is low (%.1fV). Sleeping for 10 minutes\n", mV / 1000);
    if (mqttClient.connected())
    {
      snprintf_P(jsonBuf, sizeof(jsonBuf),
        PSTR("{\"battery_level\":0,\"cleaning\":false,\"docked\":false,"
             "\"charging\":false,\"voltage\":%.2f,\"charge\":0,\"state\":\"idle\"}"),
        mV / 1000.0f);
      mqttClient.publish(getMQTTTopic(statusTopic), jsonBuf, true);
    }
    delay(200);

    // Sleep for 10 minutes
    ESP.deepSleep(600e6);
  }
#endif
}

bool parseRoombaStateFromStreamPacket(uint8_t *packet, int length, RoombaState *state)
{
  state->timestamp = millis();
  int i = 0;
  while (i < length)
  {
    switch (packet[i])
    {
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
      state->distance = packet[i + 1] * 256 + packet[i + 2];
      i += 3;
      break;
    case Roomba::SensorChargingState: // 21
      state->chargingState = packet[i + 1];
      i += 2;
      break;
    case Roomba::SensorVoltage: // 22
      state->voltage = packet[i + 1] * 256 + packet[i + 2];
      i += 3;
      break;
    case Roomba::SensorCurrent: // 23
      state->current = packet[i + 1] * 256 + packet[i + 2];
      i += 3;
      break;
    case Roomba::SensorBatteryTemperature: // 24
      state->temp = packet[i + 1];
      i += 2;
      break;
    case Roomba::SensorBatteryCharge: // 25
      state->charge = packet[i + 1] * 256 + packet[i + 2];
      i += 3;
      break;
    case Roomba::SensorBatteryCapacity: // 26
      state->capacity = packet[i + 1] * 256 + packet[i + 2];
      i += 3;
      break;
    case Roomba::SensorChargingSourcesAvailable: // 34
      state->chargingSourcesAvailable = packet[i + 1];
      i += 2;
      break;
    case Roomba::SensorOIMode: // 35
      state->OIMode = packet[i + 1];
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

void verboseLogPacket(uint8_t *packet, uint8_t length)
{
  VLOG("Packet: ");
  for (int i = 0; i < length; i++)
  {
    VLOG("%d ", packet[i]);
  }
  VLOG("\n");
}

void readSensorPacket()
{
  uint8_t packetLength;
  bool received = roomba.pollSensors(roombaPacket, sizeof(roombaPacket), &packetLength);
  if (received)
  {
    RoombaState rs = {};
    bool parsed = parseRoombaStateFromStreamPacket(roombaPacket, packetLength, &rs);
    verboseLogPacket(roombaPacket, packetLength);
    if (parsed && rs.temp != 0)
    {
      rs.cleaning = roombaState.cleaning;
      rs.docked = roombaState.docked;
      rs.returning = roombaState.returning;
      rs.paused = roombaState.paused;
      roombaState = rs;
      VLOG("Got Packet of len=%d! OIMode:%d Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", packetLength, roombaState.OIMode, roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
      roombaState.cleaning = false;
      roombaState.docked = false;
      if (roombaState.current < -400)
      {
        roombaState.cleaning = true;
      }
      else if (roombaState.current > -50)
      {
        roombaState.docked = true;
        roombaState.returning = false;
      }
    }
    else
    {
      VLOG("Failed to parse packet, packetLength:%d, Temperature:%d\n", packetLength, rs.temp);
    }
  }
}

void onOTAStart()
{
  DLOG("Starting OTA session\n");
  DLOG("Pause streaming\n");
  roomba.streamCommand(Roomba::StreamCommandPause);
  OTAStarted = true;
}

// ---------------------------------------------------------------------------
// Control web server — accessible at http://roomba.local/ in normal WiFi mode
// ---------------------------------------------------------------------------

static const char HTML_UPDATE_OK[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='15;url=/'>"
    "<style>body{font-family:sans-serif;max-width:440px;margin:0 auto;padding:12px}</style>"
    "</head><body>"
    "<h2>Updated!</h2><p>Rebooting into new firmware...</p>"
    "<p>Reloading in <span id='t'>15</span>s...</p>"
    "<script>var n=15;setInterval(function(){document.getElementById('t').textContent=--n},1000)</script>"
    "</body></html>";

// Dashboard HTML split into static fragments for streaming (avoids large stack buffer).
// HTML_PANEL_A: up to where MAC suffix goes in the title
static const char HTML_PANEL_A[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>" ROOMBA_FRIENDLY_NAME " (";
// → send macSuffix
// HTML_PANEL_B: closing title, CSS, nav, h2 opening up to second MAC suffix
static const char HTML_PANEL_B[] PROGMEM =
    ")</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:440px;margin:0 auto;padding:12px}"
    "h2{margin-bottom:6px}"
    ".st{background:#f0f0f0;border-radius:4px;padding:8px;margin-bottom:12px;font-size:.9em}"
    ".st td{padding:2px 6px}.st td:first-child{font-weight:bold;width:40%}"
    ".btns{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:12px}"
    "button{padding:9px;font-size:.9em;border:none;border-radius:4px;cursor:pointer;"
    "background:#0055aa;color:#fff}"
    ".s{background:#cc2200}.d{background:#007744}" HTML_NAV_CSS HTML_FOOTER_CSS
    "</style></head><body>"
    "<nav><span>Home</span> &nbsp;|&nbsp; <a href='/control'>Control</a>"
    " &nbsp;|&nbsp; <a href='/config'>Configuration</a>"
    " &nbsp;|&nbsp; <a href='/update'>Firmware</a></nav>"
    "<h2>" ROOMBA_FRIENDLY_NAME " (";
// → send macSuffix
// HTML_PANEL_C: close h2, open status table
static const char HTML_PANEL_C[] PROGMEM =
    ")</h2>"
    "<div class='st'><table>";
// → stream table rows individually
// HTML_PANEL_D: close status table + footer (home page end)
static const char HTML_PANEL_D[] PROGMEM =
    "</table></div>"
    HTML_FOOTER_BODY
    "</body></html>";

// HTML_CTRL_PAGE: full /control page (no meta-refresh, static)
static const char HTML_CTRL_PAGE[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>" ROOMBA_FRIENDLY_NAME " Control</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:440px;margin:0 auto;padding:12px}"
    "h2{margin-bottom:6px}"
    ".btns{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:12px}"
    "button{padding:9px;font-size:.9em;border:none;border-radius:4px;cursor:pointer;"
    "background:#0055aa;color:#fff}"
    ".s{background:#cc2200}.d{background:#007744}"
    ".drv{text-align:center;margin-bottom:12px}"
    ".dc{display:flex;justify-content:center;gap:6px;margin:3px 0}"
    ".drv button{width:56px;height:56px;font-size:1.4em;padding:0}"
    HTML_NAV_CSS HTML_FOOTER_CSS
    "</style></head><body>"
    "<nav><a href='/'>Home</a> &nbsp;|&nbsp; <span>Control</span>"
    " &nbsp;|&nbsp; <a href='/config'>Configuration</a>"
    " &nbsp;|&nbsp; <a href='/update'>Firmware</a></nav>"
    "<h2>" ROOMBA_FRIENDLY_NAME " Control</h2>"
    "<form method='POST' action='/cmd' class='btns'>"
    "<button name='action' value='clean'>Clean</button>"
    "<button name='action' value='stop' class='s'>Stop</button>"
    "<button name='action' value='dock' class='d'>Go home</button>"
    "<button name='action' value='spot'>Spot</button>"
    "<button name='action' value='locate'>Locate</button>"
    "<button name='action' value='wake'>Wake</button>"
    "</form>"
    "<div class='drv'>"
    "<div class='dc'>"
    "<button onmousedown='dr(200,32767)' onmouseup='ds()'"
    " ontouchstart='ev(event,200,32767)' ontouchend='ds()' ontouchcancel='ds()'>&#x2191;</button>"
    "</div>"
    "<div class='dc'>"
    "<button onmousedown='dr(150,1)' onmouseup='ds()'"
    " ontouchstart='ev(event,150,1)' ontouchend='ds()' ontouchcancel='ds()'>&#x21BA;</button>"
    "<button class='s' onclick='ds()'>&#x25A0;</button>"
    "<button onmousedown='dr(150,-1)' onmouseup='ds()'"
    " ontouchstart='ev(event,150,-1)' ontouchend='ds()' ontouchcancel='ds()'>&#x21BB;</button>"
    "</div>"
    "<div class='dc'>"
    "<button onmousedown='dr(-200,32767)' onmouseup='ds()'"
    " ontouchstart='ev(event,-200,32767)' ontouchend='ds()' ontouchcancel='ds()'>&#x2193;</button>"
    "</div>"
    "</div>"
    "<script>"
    "function dr(v,r){fetch('/drive',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'v='+v+'&r='+r})}"
    "function ds(){dr(0,0)}"
    "function ev(e,v,r){e.preventDefault();dr(v,r)}"
    "window.addEventListener('beforeunload',ds)"
    "</script>"
    HTML_FOOTER_BODY
    "</body></html>";

static const char *stateLabel(const String &s)
{
  if (s == "disconnected")
    return "Disconnected";
  if (s == "docked")
    return "Docked";
  if (s == "cleaning")
    return "Cleaning";
  if (s == "returning")
    return "Returning";
  if (s == "paused")
    return "Paused";
  return "Idle";
}

static void serveHomePage(ESP8266WebServer &s)
{
  int battPct = roombaState.capacity
                    ? (int)((int32_t)roombaState.charge * 100 / roombaState.capacity)
                    : 0;
  String stateS = getCurrentState();
  bool staConnected = (WiFi.status() == WL_CONNECTED);
  String ipS = staConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  bool charging = (roombaState.chargingState >= 1 && roombaState.chargingState <= 3);
  const char *roombaStatus = roombaConnected ? "Connected" : "No serial";
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macSuffix[13];
  snprintf(macSuffix, sizeof(macSuffix), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  char numBuf[12];

  s.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s.send(200, "text/html", "");
  s.sendContent_P(HTML_PANEL_A);
  s.sendContent(macSuffix);
  s.sendContent_P(HTML_PANEL_B);
  s.sendContent(macSuffix);
  s.sendContent_P(HTML_PANEL_C);

  s.sendContent_P(PSTR("<tr><td>Status</td><td>"));
  s.sendContent(stateLabel(stateS));
  s.sendContent_P(PSTR("</td></tr><tr><td>Battery</td><td>"));
  snprintf(numBuf, sizeof(numBuf), "%d%%", battPct);
  s.sendContent(numBuf);
  s.sendContent_P(PSTR("</td></tr><tr><td>Charging</td><td>"));
  s.sendContent(charging ? "Yes" : "No");
  s.sendContent_P(PSTR("</td></tr><tr><td>WiFi</td><td>"));
  if (staConnected)
  {
    s.sendContent(WiFi.SSID());
    snprintf(numBuf, sizeof(numBuf), " (%d dBm)", (int)WiFi.RSSI());
    s.sendContent(numBuf);
  }
  else
  {
    s.sendContent_P(PSTR("AP mode"));
  }
  s.sendContent_P(PSTR("</td></tr><tr><td>IP</td><td>"));
  s.sendContent(ipS);
  s.sendContent_P(PSTR("</td></tr><tr><td>Hostname</td><td>"));
  s.sendContent(cfg.hostname);
  s.sendContent_P(PSTR(".local</td></tr><tr><td>MQTT</td><td>"));
  s.sendContent(mqttClient.connected() ? "Connected" : "Offline");
  s.sendContent_P(PSTR("</td></tr><tr><td>Roomba</td><td>"));
  s.sendContent(roombaStatus);
  s.sendContent_P(PSTR("</td></tr>"));

  s.sendContent_P(HTML_PANEL_D);
}

static void handleControlPanel()
{
  serveHomePage(controlServer);
}

static void serveControlPage(ESP8266WebServer &s)
{
  s.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s.send(200, "text/html", "");
  s.sendContent_P(HTML_CTRL_PAGE);
}

static void handleControlPage()
{
  serveControlPage(controlServer);
}

static void dispatchCmd(const String &action)
{
  if (action == "clean")
    turnOn();
  else if (action == "stop")
    stop();
  else if (action == "dock")
    returnToBase();
  else if (action == "spot")
    cleanSpot();
  else if (action == "locate")
    locate();
  else if (action == "wake")
  {
    if (!roombaConnected)
      roombaDeepWakeAndReconnect();
    else
      wakeup();
  }
}

static void dispatchDrive(int16_t v, int16_t r)
{
  v = (int16_t)constrain(v, -500, 500);
  r = (int16_t)constrain(r, -32768, 32767);
  roombaWakeAndSafe();
  roomba.drive(v, r);
  if (v == 0 && r == 0) {
    delay(100);
    roomba.start();
  }
}

static void handleControlCmd()
{
  dispatchCmd(controlServer.arg("action"));
  controlServer.sendHeader(F("Location"), F("/control"));
  controlServer.send(302, F("text/plain"), "");
}

static void handleControlConfig()
{
  serveConfigPage(controlServer, false);
}

static void handleControlReset()
{
  configLoadDefaults();
  configSave();
  controlServer.sendHeader(F("Location"), F("/config"));
  controlServer.send(302, F("text/plain"), "");
  delay(300);
  ESP.restart();
}

static void handleControlSave()
{
  processConfigSave(controlServer);
  sendSavedPage(controlServer);
  delay(300);
  ESP.restart();
}

static void handleUpdateForm()
{
  serveUpdatePage(controlServer, false);
}

static void handleUpdateDone()
{
  if (Update.hasError())
  {
    OTAStarted = false;
    roomba.streamCommand(Roomba::StreamCommandResume);
    sendUpdateFailPage(controlServer, Update.getErrorString().c_str());
  }
  else
  {
    char buf[600];
    snprintf_P(buf, sizeof(buf), HTML_UPDATE_OK);
    controlServer.send(200, F("text/html"), buf);
    delay(500);
    ESP.restart();
  }
}

static void handleUpdateUpload()
{
  HTTPUpload &upload = controlServer.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    roomba.streamCommand(Roomba::StreamCommandPause);
    OTAStarted = true;
    Update.begin(ESP.getFreeSketchSpace());
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    Update.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    Update.end(true);
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    Update.end();
    OTAStarted = false;
    roomba.streamCommand(Roomba::StreamCommandResume);
  }
}

static void handleDriveCmd()
{
  int16_t v = (int16_t)constrain(controlServer.arg("v").toInt(), -500, 500);
  int16_t r = (int16_t)constrain(controlServer.arg("r").toInt(), -32768, 32767);
  roombaWakeAndSafe();
  roomba.drive(v, r);
  if (v == 0 && r == 0)
  {
    delay(100);
    roomba.start(); // back to Passive mode
  }
  controlServer.send(200, F("text/plain"), F("OK"));
}

void setupControlServer()
{
  controlServer.on("/", HTTP_GET, handleControlPanel);
  controlServer.on("/control", HTTP_GET, handleControlPage);
  controlServer.on("/cmd", HTTP_POST, handleControlCmd);
  controlServer.on("/drive", HTTP_POST, handleDriveCmd);
  controlServer.on("/config", HTTP_GET, handleControlConfig);
  controlServer.on("/save", HTTP_POST, handleControlSave);
  controlServer.on("/reset", HTTP_POST, handleControlReset);
  controlServer.on("/update", HTTP_GET, handleUpdateForm);
  controlServer.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  controlServer.begin();
}

void setup()
{
  // High-impedence on the BRC_PIN
  pinMode(BRC_PIN, INPUT);

  // Sleep immediately if ENABLE_ADC_SLEEP and the battery is low
  sleepIfNecessary();

  // Load runtime config from EEPROM (falls back to compile-time defaults on first boot)
  configLoad();

  // Set Hostname.
  String hostname(cfg.hostname);
  WiFi.persistent(false);              // Don't write credentials to flash on every connect
  WiFi.mode(WIFI_STA);                 // Force STA mode — SDK may boot in AP mode if portal ran last
  delay(100);                          // Let radio settle after soft reset (ESP.restart())
  WiFi.setAutoReconnect(false);        // Managed manually; re-enabled after first successful connect
  WiFi.setSleepMode(WIFI_MODEM_SLEEP); // Modem sleep between beacons (~20-30% WiFi power saving)
  WiFi.hostname(hostname);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_password);

  {
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(200);
      yield();
      if (millis() - wifiStart > WIFI_CONNECT_TIMEOUT_MS)
      {
        // WiFi unavailable — enter config portal so credentials can be updated
        portalStart();
        portalSetHandlers(serveHomePage, dispatchCmd, serveControlPage, dispatchDrive);
        while (portalActive())
        {
          portalHandle();
          yield();
        }
        ESP.restart();
        return; // never reached
      }
    }
  }
  WiFi.setAutoReconnect(true); // re-enable SDK auto-reconnect after first connect

  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.onStart(onOTAStart);
  ArduinoOTA.onError([](ota_error_t error)
                     {
    OTAStarted = false;
    roomba.streamCommand(Roomba::StreamCommandResume); });
  ArduinoOTA.begin();

  // Synchronize time using SNTP. This is necessary to verify that
  // the TLS certificates offered by the server are currently valid.
  setDateTime();
#if USE_SSL
  wifiClient.setCACert_P(caCert, caCertLen);
#endif

  mqttClient.setServer(cfg.mqtt_server, cfg.mqtt_port);
  mqttClient.setKeepAlive(60);
  mqttClient.setCallback(mqttCallback);

#if LOGGING
  Debug.begin((const char *)hostname.c_str());
  Debug.setResetCmdEnabled(true);
  Debug.setCallBackProjectCmds(debugCallback);
  Debug.setSerialEnabled(false);
#endif

  initRoombaStream();

  // Start control web server (accessible at http://roomba.local/)
  setupControlServer();
}

void reconnect()
{
  DLOG("Attempting MQTT connection...\n");
  // Attempt to connect. LWT retain=true so broker keeps "offline" for HA restarts.
  if (mqttClient.connect(getEntityID(HOSTNAME_PREFIX), cfg.mqtt_user, cfg.mqtt_password, getMQTTTopic(lwtTopic), 0, true, lwtMessage))
  {
    DLOG("MQTT connected\n");
    // Publish "online" so HA marks the entity as available
    mqttClient.publish(getMQTTTopic(lwtTopic), "online", true);
    mqttClient.subscribe(getMQTTTopic(commandTopic));
    mqttClient.subscribe(getMQTTTopic(driveTopic));
    mqttClient.subscribe(getMQTTTopic(songTopic));
  }
  else
  {
    DLOG("MQTT failed rc=%d try again in 5 seconds\n", mqttClient.state());
#if USE_SSL
    char buf[256];
    int ernum = wifiClient.getLastSSLError(buf, 256);
    DLOG("MQTT SSL Error: %d - %s\n", ernum, buf);
#endif
  }
}

char *getSensorDiscoveryTopic(const char *component, const char *entitySuffix)
{
  static char sensorTopic[200];
  sprintf(sensorTopic, "%s/%s/%s%s/config",
          MQTT_DISCOVERY, component, getEntityID(), entitySuffix);
  return sensorTopic;
}

void sendConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":null,\"has_entity_name\":true,"
         "\"unique_id\":\"%s\",\"object_id\":\"%s\","
         "\"~\":\"" MQTT_TOPIC_BASE "%s\","
         "\"stat_t\":\"~/state\",\"cmd_t\":\"~/command\","
         "\"send_cmd_t\":\"~/command\",\"json_attr_t\":\"~/state\","
         "\"avty_t\":\"~/LWT\",\"pl_avail\":\"online\","
         "\"pl_not_avail\":\"offline\","
         "\"sup_feat\":[\"start\",\"stop\",\"pause\",\"return_home\","
                       "\"locate\",\"clean_spot\",\"send_command\"],"
         "\"dev\":{\"name\":\"" ROOMBA_MODEL "\","
                  "\"ids\":[\"%s\"],\"mf\":\"iRobot\","
                  "\"mdl\":\"" ROOMBA_MODEL "\","
                  "\"cu\":\"http://%s.local\"}}"),
    eid, eid, eid, eid, cfg.hostname);
  DLOG("Reporting vacuum config: %s\n", jsonBuf);
  mqttClient.publish(getMQTTTopic(configTopic), jsonBuf, true);
}

void sendBatterySensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending battery sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Battery\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_battery\",\"object_id\":\"%s_battery\","
         "\"device_class\":\"battery\",\"state_class\":\"measurement\","
         "\"unit_of_measurement\":\"%%\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.battery_level}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting battery sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_battery"), jsonBuf, true);
}

void sendChargingSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending charging sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Charging\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_charging\",\"object_id\":\"%s_charging\","
         "\"device_class\":\"battery_charging\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.charging | lower}}\","
         "\"pl_on\":\"true\",\"pl_off\":\"false\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting charging sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("binary_sensor", "_charging"), jsonBuf, true);
}

void sendRSSISensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending RSSI sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Signal\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_rssi\",\"object_id\":\"%s_rssi\","
         "\"device_class\":\"signal_strength\",\"state_class\":\"measurement\","
         "\"unit_of_measurement\":\"dBm\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.rssi}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting RSSI sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_rssi"), jsonBuf, true);
}

void sendIPSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending IP sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"IP\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_ip\",\"object_id\":\"%s_ip\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.ip}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting IP sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_ip"), jsonBuf, true);
}

void sendConnectedSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending connected sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Serial connected\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_connected\",\"object_id\":\"%s_connected\","
         "\"device_class\":\"connectivity\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.roomba_connected | lower}}\","
         "\"pl_on\":\"true\",\"pl_off\":\"false\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting connected sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("binary_sensor", "_connected"), jsonBuf, true);
}

void sendStateSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending state sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Estado\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_state\",\"object_id\":\"%s_state\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.state}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting state sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_state"), jsonBuf, true);
}

void sendMACSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending MAC sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"MAC\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_mac\",\"object_id\":\"%s_mac\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.mac}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting MAC sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_mac"), jsonBuf, true);
}

void sendSSIDSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending SSID sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"SSID\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_ssid\",\"object_id\":\"%s_ssid\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.ssid}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting SSID sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_ssid"), jsonBuf, true);
}

void sendBatteryTempSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending battery temp sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Battery temperature\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_battery_temp\",\"object_id\":\"%s_battery_temp\","
         "\"device_class\":\"temperature\",\"state_class\":\"measurement\","
         "\"unit_of_measurement\":\"\\u00b0C\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.batteryTemperature}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting battery temp sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_battery_temp"), jsonBuf, true);
}

void sendVoltageSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending voltage sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Voltage\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_voltage\",\"object_id\":\"%s_voltage\","
         "\"device_class\":\"voltage\",\"state_class\":\"measurement\","
         "\"unit_of_measurement\":\"mV\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.voltage}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting voltage sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_voltage"), jsonBuf, true);
}

void sendCurrentSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending current sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Current\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_current\",\"object_id\":\"%s_current\","
         "\"device_class\":\"current\",\"state_class\":\"measurement\","
         "\"unit_of_measurement\":\"mA\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.current}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting current sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_current"), jsonBuf, true);
}

void sendHostnameSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending hostname sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Hostname\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_hostname\",\"object_id\":\"%s_hostname\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.hostname}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting hostname sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_hostname"), jsonBuf, true);
}

void sendOIModeSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending OI mode sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"OI Mode\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_oi_mode\",\"object_id\":\"%s_oi_mode\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.OIMode}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting OI mode sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_oi_mode"), jsonBuf, true);
}

void sendChargeSensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending charge sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Charge\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_charge\",\"object_id\":\"%s_charge\","
         "\"state_class\":\"measurement\","
         "\"unit_of_measurement\":\"mAh\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.charge}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting charge sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_charge"), jsonBuf, true);
}

void sendCapacitySensorConfig()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending capacity sensor config\n");
    return;
  }
  const char *eid = getEntityID();
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"name\":\"Capacity\",\"has_entity_name\":true,"
         "\"unique_id\":\"%s_capacity\",\"object_id\":\"%s_capacity\","
         "\"state_class\":\"measurement\","
         "\"unit_of_measurement\":\"mAh\","
         "\"stat_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_STATE_TOPIC "\","
         "\"val_tpl\":\"{{value_json.capacity}}\","
         "\"avty_t\":\"" MQTT_TOPIC_BASE "%s/" MQTT_LWT_TOPIC "\","
         "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         "\"dev\":{\"ids\":[\"%s\"],\"name\":\"" ROOMBA_MODEL "\","
                  "\"mf\":\"iRobot\",\"mdl\":\"" ROOMBA_MODEL "\"}}"),
    eid, eid, eid, eid, eid);
  DLOG("Reporting capacity sensor config: %s\n", jsonBuf);
  mqttClient.publish(getSensorDiscoveryTopic("sensor", "_capacity"), jsonBuf, true);
}

String getCurrentState()
{
  if (!roombaConnected)
    return "disconnected";
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

void sendStatus()
{
  if (!mqttClient.connected())
  {
    DLOG("MQTT Disconnected, not sending status\n");
    return;
  }
  DLOG("Reporting packet Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
  int16_t batteryLevel = roombaState.capacity ? (roombaState.charge * 100) / roombaState.capacity : 0;
  boolean docked = roombaState.chargingSourcesAvailable == Roomba::ChargeAvailableDock;
  boolean isCharging = roombaState.chargingState == Roomba::ChargeStateReconditioningCharging ||
                       roombaState.chargingState == Roomba::ChargeStateFullCharging ||
                       roombaState.chargingState == Roomba::ChargeStateTrickleCharging;
  String curState = getCurrentState();
  char ipStr[16];
  IPAddress ip = WiFi.localIP();
  snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%s", getMAC(":"));
  snprintf_P(jsonBuf, sizeof(jsonBuf),
    PSTR("{\"battery_level\":%d,\"cleaning\":%s,\"returning\":%s,"
         "\"docked\":%s,\"charging\":%s,\"chargingState\":%d,"
         "\"voltage\":%d,\"current\":%d,\"charge\":%d,\"capacity\":%d,"
         "\"distance\":%d,\"batteryTemperature\":%d,"
         "\"chargingSourcesAvailable\":%d,\"OIMode\":%d,"
         "\"state\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,"
         "\"ip\":\"%s\",\"hostname\":\"%s\","
         "\"mac\":\"%s\",\"roomba_connected\":%s}"),
    (int)batteryLevel,
    boolStr(roombaState.cleaning), boolStr(roombaState.returning),
    boolStr((bool)docked), boolStr((bool)isCharging),
    (int)roombaState.chargingState, (int)roombaState.voltage,
    (int)roombaState.current, (int)roombaState.charge, (int)roombaState.capacity,
    (int)roombaState.distance, (int)roombaState.temp,
    (int)roombaState.chargingSourcesAvailable, (int)roombaState.OIMode,
    curState.c_str(), WiFi.SSID().c_str(), (int)WiFi.RSSI(),
    ipStr, WiFi.getHostname() ? WiFi.getHostname() : "",
    macStr, boolStr(roombaConnected));
  mqttClient.publish(getMQTTTopic(statusTopic), jsonBuf);
}

void loop()
{
  // Important callbacks that _must_ happen every cycle
  ArduinoOTA.handle();
  yield();
#if LOGGING
  Debug.handle();
#endif

  // Handle config portal if active (entered from setup() timeout or loop() watchdog)
  if (portalActive())
  {
    portalHandle();
    return;
  }

  // Skip all other logic if we're running an OTA update
  if (OTAStarted)
  {
    return;
  }

  // Serve control panel requests
  controlServer.handleClient();

  long now = millis();
  // WiFi watchdog: if WiFi dropped, attempt reconnect with exponential backoff
  static uint32_t wifiLostAt = 0;
  static uint32_t wifiNextReconnect = 0;
  if (WiFi.status() != WL_CONNECTED)
  {
    DLOG("WiFi disconnected, reconnecting...\n");
    if (!wifiLostAt)
      wifiLostAt = (uint32_t)now;
    if ((uint32_t)now >= wifiNextReconnect)
    {
      WiFi.begin(cfg.wifi_ssid, cfg.wifi_password);
      uint32_t elapsed = (uint32_t)now - wifiLostAt;
      uint32_t backoff = elapsed < 10000 ? 5000 : min((uint32_t)60000, elapsed / 4);
      wifiNextReconnect = (uint32_t)now + backoff;
    }
    if ((uint32_t)now - wifiLostAt > WIFI_RECONNECT_PORTAL_MS)
    {
      wifiLostAt = 0;
      wifiNextReconnect = 0;
      controlServer.stop();
      roomba.streamCommand(Roomba::StreamCommandPause);
      portalStart();
      portalSetHandlers(serveHomePage, dispatchCmd, serveControlPage, dispatchDrive);
    }
    return;
  }
  wifiLostAt = 0;
  wifiNextReconnect = 0;
  // If MQTT client can't connect to broker, then reconnect
  if (!mqttClient.connected() && (now - lastConnectTime) > RECONNECT_FREQ)
  {
    DLOG("Reconnecting MQTT\n");
    lastConnectTime = now;
    lastConfigSend = now;
    reconnect();
    sendConfig();
    sendBatterySensorConfig();
    sendChargingSensorConfig();
    sendRSSISensorConfig();
    sendIPSensorConfig();
    sendConnectedSensorConfig();
    sendStateSensorConfig();
    sendMACSensorConfig();
    sendSSIDSensorConfig();
    sendBatteryTempSensorConfig();
    sendVoltageSensorConfig();
    sendCurrentSensorConfig();
    sendHostnameSensorConfig();
    sendOIModeSensorConfig();
    sendChargeSensorConfig();
    sendCapacitySensorConfig();
  }
  else if ((now - lastConfigSend) > CONFIG_SEND_FREQ)
  {
    lastConfigSend = now;
    sendConfig();
    sendBatterySensorConfig();
    sendChargingSensorConfig();
    sendRSSISensorConfig();
    sendIPSensorConfig();
    sendConnectedSensorConfig();
    sendStateSensorConfig();
    sendMACSensorConfig();
    sendSSIDSensorConfig();
    sendBatteryTempSensorConfig();
    sendVoltageSensorConfig();
    sendCurrentSensorConfig();
    sendHostnameSensorConfig();
    sendOIModeSensorConfig();
    sendChargeSensorConfig();
    sendCapacitySensorConfig();
  }

#if KEEP_ROOMBA_AWAKE
  // Keep the Roomba awake at fixed intervals.
  // Must send an actual OI command (not just a BRC pulse) every cycle to reset
  // the Roomba's OI inactivity timer; otherwise it sleeps after ~5 minutes.
  if (now - lastWakeupTime > WAKEUP_FREQ)
  {
    lastWakeupTime = now;
    if (!roombaConnected)
    {
      // Roomba in deep sleep — aggressive 3×500ms wake + full OI re-init
      roombaDeepWakeAndReconnect();
    }
    else
    {
      // Roomba awake — short BRC pulse (safe, no baud-rate change) +
      // OI stream command to reset the inactivity timer
      if (roombaState.docked)
        wakeOnDock();
      else
        wakeup();
      roomba.stream(sensors, sizeof(sensors));
    }
  }
#endif

  // Report status at full rate while active, slower when docked/idle to save radio
  uint32_t statusFreq = (roombaState.cleaning || roombaState.returning)
                         ? (uint32_t)STATUS_REPORT_FREQ
                         : (uint32_t)STATUS_REPORT_FREQ * 3UL;
  if ((uint32_t)now - (uint32_t)lastStateMsgTime > statusFreq)
  {
    lastStateMsgTime = now;
    if (now - roombaState.timestamp > 30000 || roombaState.sent)
    {
      DLOG("Roomba state stale (%.1fs old)\n", (now - roombaState.timestamp) / 1000.0);
      if (!roombaConnected)
      {
        // OI is in OFF mode after deep sleep — full re-init before stream will work
        DLOG("Roomba disconnected, re-initialising stream\n");
        initRoombaStream();
        sendStatus();
      }
      else
      {
        DLOG("Request stream\n");
        roomba.stream(sensors, sizeof(sensors));
      }
    }
    else
    {
      sendStatus();
      roombaState.sent = true;
    }
    sleepIfNecessary();
  }

  readSensorPacket();
  roombaConnected = (roombaState.timestamp != 0 && (millis() - roombaState.timestamp) < 60000);
  mqttClient.loop();
}
