#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

#include "can_bus.h"
#include "config.h"
#include "web_server.h"
#include "serial_gateway.h"
#include "status_output.h"

WiFiEventHandler wifiApStationConnectedHandler;
WiFiEventHandler wifiApStationDisconnectedHandler;

unsigned long lastStatusAtMs = 0;

void setup() {
  pinMode(Config::MCP2515_INT_PIN, INPUT);

  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("TLP-Klappen NodeMCU"));
  Serial.print(F("CAN prep: cs="));
  Serial.print(Config::MCP2515_CS_PIN);
  Serial.print(F(" int="));
  Serial.println(Config::MCP2515_INT_PIN);

  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS mount failed! Run: pio run -t uploadfs"));
  }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);

  wifiApStationConnectedHandler = WiFi.onSoftAPModeStationConnected(
    [](const WiFiEventSoftAPModeStationConnected &e) {
      if (serialProtocolActive()) return;
      Serial.print(F("AP: station connected aid="));
      Serial.printf("%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
        e.aid, e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
    });

  wifiApStationDisconnectedHandler = WiFi.onSoftAPModeStationDisconnected(
    [](const WiFiEventSoftAPModeStationDisconnected &e) {
      if (serialProtocolActive()) return;
      Serial.print(F("AP: station disconnected aid="));
      Serial.println(e.aid);
    });

  const bool started = WiFi.softAP(
    Config::AP_SSID,
    Config::AP_PASSWORD,
    Config::AP_CHANNEL,
    Config::AP_HIDDEN,
    Config::AP_MAX_CLIENTS
  );

  Serial.print(F("softAP start result="));
  Serial.println(started ? F("ok") : F("failed"));
  Serial.print(F("AP MAC="));
  Serial.println(WiFi.softAPmacAddress());

  printApStatus();
  WiFi.printDiag(Serial);

  startWebServer();

  if (runCanControllerSelfTest()) {
    switchToCanNormalMode();
  }

  Serial.print(F("Web URL: http://"));
  Serial.println(WiFi.softAPIP());
}

void loop() {
  const unsigned long loopStartedAtUs = micros();
  const unsigned long nowMs = millis();

  cleanupExpiredPendingCommands(nowMs);
  pollSerialGateway();

  if (nowMs - lastStatusAtMs >= Config::STATUS_INTERVAL_MS) {
    lastStatusAtMs = nowMs;
    printApStatus();
  }

  pollCanFrames();
  printCanStatus(nowMs);
  streamApStatus();

  yield();
  const unsigned long durationUs = micros() - loopStartedAtUs;
  if (durationUs > maxLoopDurationUs) maxLoopDurationUs = durationUs;
}
