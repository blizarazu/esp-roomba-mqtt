#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <config.h>
#include <secrets.h>

void setup()
{
    String hostname(HOSTNAME);
    WiFi.hostname(hostname);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
    }

    ArduinoOTA.setHostname((const char *)hostname.c_str());
    ArduinoOTA.begin();
}

void loop()
{
    ArduinoOTA.handle();
}