#include "config_manager.h"
#include "config.h"
#include <ESP8266WiFi.h>
#include <Updater.h>
#if PORTAL_CAPTIVE_DNS
#include <DNSServer.h>
#endif

DeviceConfig cfg;

// ---------------------------------------------------------------------------
// EEPROM helpers
// ---------------------------------------------------------------------------

static uint32_t computeChecksum()
{
    uint32_t xorVal = 0;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&cfg);
    size_t len = sizeof(cfg) - sizeof(cfg.checksum);
    for (size_t i = 0; i < len; i++)
    {
        xorVal ^= p[i];
        xorVal = (xorVal << 1) | (xorVal >> 31); // rotate left 1
    }
    return xorVal;
}

void configLoadDefaults()
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    cfg.version = CONFIG_VERSION;
    strlcpy(cfg.wifi_ssid, WIFI_SSID, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_password, WIFI_PASSWORD, sizeof(cfg.wifi_password));
    strlcpy(cfg.mqtt_server, MQTT_SERVER, sizeof(cfg.mqtt_server));
    cfg.mqtt_port = MQTT_PORT;
    strlcpy(cfg.mqtt_user, MQTT_USER, sizeof(cfg.mqtt_user));
    strlcpy(cfg.mqtt_password, MQTT_PASSWORD, sizeof(cfg.mqtt_password));
    strlcpy(cfg.hostname, HOSTNAME, sizeof(cfg.hostname));
    strlcpy(cfg.mqtt_discovery, MQTT_DISCOVERY, sizeof(cfg.mqtt_discovery));
    strlcpy(cfg.ntp_server1, NTP_SERVER_1, sizeof(cfg.ntp_server1));
    strlcpy(cfg.ntp_server2, NTP_SERVER_2, sizeof(cfg.ntp_server2));
    cfg.checksum = computeChecksum();
}

bool configLoad()
{
    EEPROM.begin(sizeof(DeviceConfig));
    EEPROM.get(0, cfg);
    if (cfg.magic != CONFIG_MAGIC ||
        cfg.version != CONFIG_VERSION ||
        cfg.checksum != computeChecksum())
    {
        configLoadDefaults();
        return false; // first boot or corruption
    }
    return true;
}

void configSave()
{
    cfg.magic = CONFIG_MAGIC;
    cfg.version = CONFIG_VERSION;
    cfg.checksum = computeChecksum();
    EEPROM.put(0, cfg);
    EEPROM.commit();
}

// ---------------------------------------------------------------------------
// Shared web helpers
// ---------------------------------------------------------------------------

// HTML head stored in flash (PROGMEM). The form is streamed in fragments to
// avoid a large render buffer: sendContent_P() reads directly from flash and
// dynamic values are sent inline. No %% escaping needed (no snprintf).
static const char HTML_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Roomba</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:420px;margin:0 auto;padding:12px}"
    "label{font-weight:bold;font-size:.9em;display:block;margin-top:8px}"
    "input{width:100%;box-sizing:border-box;padding:5px;margin-top:2px}"
    ".hint{font-size:.8em;color:#666}"
    "fieldset{border:1px solid #ccc;border-radius:4px;margin-top:12px;padding:8px}"
    "legend{font-weight:bold;padding:0 4px}"
    "input[type=submit]{margin-top:14px;padding:8px;background:#0066cc;"
    "color:#fff;border:none;border-radius:4px;font-size:1em;cursor:pointer}"
    "input.rst{background:#cc2200}"
    "input[type=file]{width:100%;margin:12px 0}" HTML_NAV_CSS HTML_FOOTER_CSS
    "</style></head><body>";

static const char HTML_SAVED_FMT[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='10;url=/'>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:0 auto;padding:12px}</style>"
    "</head><body>"
    "<h2>Saved!</h2><p>Device is restarting.</p>"
    "<p>Reloading in <span id='t'>10</span>s...</p>"
    "<script>var n=10;setInterval(function(){document.getElementById('t').textContent=--n},1000)</script>"
    "</body></html>";

static const char HTML_PORTAL_UPDATE_OK_FMT[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='15;url=/'>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:0 auto;padding:12px}</style>"
    "</head><body>"
    "<h2>Update OK!</h2><p>Rebooting into new firmware...</p>"
    "<p>Reloading in <span id='t'>15</span>s...</p>"
    "<script>var n=15;setInterval(function(){document.getElementById('t').textContent=--n},1000)</script>"
    "</body></html>";

void sendSavedPage(ESP8266WebServer &s)
{
    char buf[600];
    snprintf_P(buf, sizeof(buf), HTML_SAVED_FMT);
    s.send(200, "text/html", buf);
}

void sendUpdateFailPage(ESP8266WebServer &s, const char *error)
{
    s.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s.send(500, "text/html", "");
    s.sendContent_P(PSTR(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
        "<body style='font-family:sans-serif;max-width:420px;margin:0 auto;padding:12px'>"
        "<h2>Update failed</h2><p>"));
    s.sendContent(error);
    s.sendContent_P(PSTR("</p><p><a href='/update'>Try again</a></p></body></html>"));
}

// active: 0=Home, 1=Control, 2=Configuration, 3=Firmware
static void sendNav(ESP8266WebServer &s, bool /*isPortal*/, uint8_t active)
{
    s.sendContent_P(PSTR("<nav>"));
    if (active == 0)
        s.sendContent_P(PSTR("<span>Home</span>"));
    else
        s.sendContent_P(PSTR("<a href='/'>Home</a>"));
    s.sendContent_P(PSTR(" &nbsp;|&nbsp; "));
    s.sendContent_P(PSTR("<a href='/control'>Control</a>"));
    s.sendContent_P(PSTR(" &nbsp;|&nbsp; "));
    if (active == 2)
        s.sendContent_P(PSTR("<span>Configuration</span>"));
    else
        s.sendContent_P(PSTR("<a href='/config'>Configuration</a>"));
    s.sendContent_P(PSTR(" &nbsp;|&nbsp; "));
    if (active == 3)
        s.sendContent_P(PSTR("<span>Firmware</span>"));
    else
        s.sendContent_P(PSTR("<a href='/update'>Firmware</a>"));
    s.sendContent_P(PSTR("</nav>"));
}

void serveConfigPage(ESP8266WebServer &s, bool isPortal)
{
    s.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s.send(200, "text/html", "");
    s.sendContent_P(HTML_HEAD);
    sendNav(s, isPortal, 2);
    s.sendContent_P(PSTR(
        "<h2>Configuration</h2>"
        "<form method='POST' action='/save'>"
        "<fieldset><legend>WiFi</legend>"
        "<label>SSID</label><input name='ssid' maxlength='32' value='"));
    if (cfg.wifi_ssid[0])
        s.sendContent(cfg.wifi_ssid);
    s.sendContent_P(PSTR(
        "'><label>Password</label>"
        "<input type='password' name='pass' maxlength='64'"
        " oninput=\"this.nextElementSibling.value='1'\" value='"));
    if (cfg.wifi_password[0] != '\0')
        s.sendContent_P(PSTR("--------"));
    s.sendContent_P(PSTR(
        "'><input type='hidden' name='pass_ch' value='0'>"
        "<label>Hostname <span class='hint'>(mDNS / OTA)</span></label>"
        "<input name='hn' maxlength='32' value='"));
    if (cfg.hostname[0])
        s.sendContent(cfg.hostname);
    s.sendContent_P(PSTR(
        "'></fieldset>"
        "<fieldset><legend>MQTT</legend>"
        "<label>Server</label><input name='ms' maxlength='64' value='"));
    if (cfg.mqtt_server[0])
        s.sendContent(cfg.mqtt_server);
    s.sendContent_P(PSTR(
        "'><label>Port</label>"
        "<input name='mp' type='number' min='1' max='65535' value='"));
    char portbuf[8];
    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)cfg.mqtt_port);
    s.sendContent(portbuf);
    s.sendContent_P(PSTR(
        "'><label>User</label><input name='mu' maxlength='32' value='"));
    if (cfg.mqtt_user[0])
        s.sendContent(cfg.mqtt_user);
    s.sendContent_P(PSTR(
        "'><label>Password</label>"
        "<input type='password' name='mpw' maxlength='64'"
        " oninput=\"this.nextElementSibling.value='1'\" value='"));
    if (cfg.mqtt_password[0] != '\0')
        s.sendContent_P(PSTR("--------"));
    s.sendContent_P(PSTR(
        "'><input type='hidden' name='mpw_ch' value='0'>"
        "<label>Discovery Prefix</label><input name='dp' maxlength='32' value='"));
    if (cfg.mqtt_discovery[0])
        s.sendContent(cfg.mqtt_discovery);
    s.sendContent_P(PSTR(
        "'></fieldset>"
        "<fieldset><legend>NTP</legend>"
        "<label>Server 1</label><input name='n1' maxlength='32' value='"));
    if (cfg.ntp_server1[0])
        s.sendContent(cfg.ntp_server1);
    s.sendContent_P(PSTR(
        "'><label>Server 2</label><input name='n2' maxlength='32' value='"));
    if (cfg.ntp_server2[0])
        s.sendContent(cfg.ntp_server2);
    s.sendContent_P(PSTR(
        "'></fieldset>"
        "<br><input type='submit' value='Save and Restart'>"
        "</form>"
        "<form method='POST' action='/reset' style='margin-top:8px'>"
        "<input type='submit' class='rst' value='Reset to defaults'>"
        "</form>" HTML_FOOTER_BODY
        "</body></html>"));
}

void serveUpdatePage(ESP8266WebServer &s, bool isPortal)
{
    s.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s.send(200, "text/html", "");
    s.sendContent_P(HTML_HEAD);
    sendNav(s, isPortal, 3);
    s.sendContent_P(PSTR(
        "<h2>Firmware update</h2>"
        "<p>Upload the <b>firmware.bin</b> file.<br></p>"
        "<form method='POST' action='/update' enctype='multipart/form-data'>"
        "<input type='file' name='firmware' accept='.bin'>"
        "<br><input type='submit' value='Flash'>"
        "</form>" HTML_FOOTER_BODY
        "</body></html>"));
}

void processConfigSave(ESP8266WebServer &s)
{
    strlcpy(cfg.wifi_ssid, s.arg("ssid").c_str(), sizeof(cfg.wifi_ssid));
    strlcpy(cfg.mqtt_server, s.arg("ms").c_str(), sizeof(cfg.mqtt_server));
    strlcpy(cfg.mqtt_user, s.arg("mu").c_str(), sizeof(cfg.mqtt_user));
    strlcpy(cfg.hostname, s.arg("hn").c_str(), sizeof(cfg.hostname));
    strlcpy(cfg.mqtt_discovery, s.arg("dp").c_str(), sizeof(cfg.mqtt_discovery));
    strlcpy(cfg.ntp_server1, s.arg("n1").c_str(), sizeof(cfg.ntp_server1));
    strlcpy(cfg.ntp_server2, s.arg("n2").c_str(), sizeof(cfg.ntp_server2));

    String portStr = s.arg("mp");
    if (portStr.length() > 0)
    {
        int p = portStr.toInt();
        if (p > 0 && p <= 65535)
            cfg.mqtt_port = (uint16_t)p;
    }
    if (s.arg("pass_ch") == "1")
        strlcpy(cfg.wifi_password, s.arg("pass").c_str(), sizeof(cfg.wifi_password));
    if (s.arg("mpw_ch") == "1")
        strlcpy(cfg.mqtt_password, s.arg("mpw").c_str(), sizeof(cfg.mqtt_password));
    configSave();
}

// ---------------------------------------------------------------------------
// Portal (AP fallback mode)
// ---------------------------------------------------------------------------

static ESP8266WebServer *_server = nullptr;
static bool _portalActive = false;
static uint32_t _portalStartMs = 0;
#if PORTAL_CAPTIVE_DNS
static DNSServer *_dns = nullptr;
#endif

static PortalHomeHandler _homeHandler    = nullptr;
static PortalCmdHandler  _cmdHandler     = nullptr;
static PortalHomeHandler _controlHandler = nullptr;
static PortalDriveHandler _driveHandler  = nullptr;

void portalSetHandlers(PortalHomeHandler home, PortalCmdHandler cmd,
                       PortalHomeHandler control, PortalDriveHandler drive)
{
    _homeHandler    = home;
    _cmdHandler     = cmd;
    _controlHandler = control;
    _driveHandler   = drive;
}

static void handleRoot()
{
    if (_homeHandler)
        _homeHandler(*_server);
    else
        serveConfigPage(*_server, true);
}

static void handlePortalConfig()
{
    serveConfigPage(*_server, true);
}

static void handlePortalCmd()
{
    if (_cmdHandler)
        _cmdHandler(_server->arg("action"));
    _server->sendHeader(F("Location"), F("/control"));
    _server->send(302, F("text/plain"), "");
}

static void handlePortalControl()
{
    if (_controlHandler)
        _controlHandler(*_server);
    else
        serveConfigPage(*_server, true);
}

static void handlePortalDrive()
{
    if (_driveHandler)
        _driveHandler((int16_t)_server->arg("v").toInt(),
                      (int16_t)_server->arg("r").toInt());
    _server->send(200, F("text/plain"), F("OK"));
}

static void handleSave()
{
    processConfigSave(*_server);
    sendSavedPage(*_server);
    delay(300);
    ESP.restart(); // always restart after save to apply new WiFi/MQTT settings
}

static void handleReset()
{
    configLoadDefaults();
    configSave();
    sendSavedPage(*_server);
    delay(300);
    ESP.restart();
}

static void handlePortalUpdateForm()
{
    serveUpdatePage(*_server, true);
}

static void handlePortalUpdateDone()
{
    if (Update.hasError())
    {
        sendUpdateFailPage(*_server, Update.getErrorString().c_str());
    }
    else
    {
        char buf[600];
        snprintf_P(buf, sizeof(buf), HTML_PORTAL_UPDATE_OK_FMT);
        _server->send(200, "text/html", buf);
        delay(500);
        ESP.restart();
    }
}

static void handlePortalUpdateUpload()
{
    HTTPUpload &upload = _server->upload();
    if (upload.status == UPLOAD_FILE_START)
    {
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
    }
}

static void handlePortalNotFound()
{
    _server->sendHeader(F("Location"), F("/"));
    _server->send(302, F("text/plain"), "");
}

static void portalStop()
{
    if (_server)
    {
        _server->stop();
        delete _server;
        _server = nullptr;
    }
#if PORTAL_CAPTIVE_DNS
    if (_dns)
    {
        _dns->stop();
        delete _dns;
        _dns = nullptr;
    }
#endif
    WiFi.softAPdisconnect(true);
    _portalActive = false;
}

void portalStart()
{
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);

    // Build AP SSID: "roomba-setup-AABBCC" (last 3 MAC bytes)
    uint8_t mac[6];
    WiFi.softAPmacAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "%s%02x%02x%02x",
             PORTAL_AP_PREFIX, mac[3], mac[4], mac[5]);

    {
        IPAddress apIP, gw, sn(255, 255, 255, 0);
        apIP.fromString(PORTAL_AP_IP);
        gw = apIP;
        WiFi.softAPConfig(apIP, gw, sn);
    }
    WiFi.softAP(apName); // open AP, no password

#if PORTAL_CAPTIVE_DNS
    _dns = new DNSServer();
    _dns->start(53, "*", WiFi.softAPIP());
#endif

    _server = new ESP8266WebServer(80);
    _server->on("/", HTTP_GET, handleRoot);
    _server->on("/control", HTTP_GET, handlePortalControl);
    _server->on("/config", HTTP_GET, handlePortalConfig);
    _server->on("/cmd", HTTP_POST, handlePortalCmd);
    _server->on("/drive", HTTP_POST, handlePortalDrive);
    _server->on("/save", HTTP_POST, handleSave);
    _server->on("/reset", HTTP_POST, handleReset);
    _server->on("/update", HTTP_GET, handlePortalUpdateForm);
    _server->on("/update", HTTP_POST, handlePortalUpdateDone, handlePortalUpdateUpload);
    _server->onNotFound(handlePortalNotFound);
    _server->begin();

    _portalActive = true;
    _portalStartMs = millis();
}

void portalHandle()
{
    if (!_portalActive)
        return;

#if PORTAL_CAPTIVE_DNS
    _dns->processNextRequest();
#endif
    _server->handleClient();

    if (millis() - _portalStartMs > PORTAL_TIMEOUT_MS)
    {
        portalStop();
        ESP.restart(); // restart to return to STA mode after timeout
    }
}

bool portalActive()
{
    return _portalActive;
}
