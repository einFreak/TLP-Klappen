#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

const char AP_SSID[] = "TLP-Klappen-Test";
const char AP_PASSWORD[] = "test12345"; // >= 8 chars for WPA2
const uint8_t AP_CHANNEL = 1;
const bool AP_HIDDEN = false;
const uint8_t AP_MAX_CLIENTS = 4;
const uint8_t RELAY_PIN = D7;
const bool RELAY_ACTIVE_HIGH = true;
const uint8_t ONBOARD_LED_PIN = LED_BUILTIN;
const bool ONBOARD_LED_ACTIVE_LOW = true;

unsigned long lastStatusAtMs = 0;
const unsigned long STATUS_INTERVAL_MS = 3000;
bool flapIsUp = false;

AsyncWebServer webServer(80);
AsyncEventSource webEvents("/events");

void setFlap(bool moveUp)
{
  flapIsUp = moveUp;
  digitalWrite(RELAY_PIN, moveUp == RELAY_ACTIVE_HIGH ? HIGH : LOW);
  digitalWrite(ONBOARD_LED_PIN, moveUp == ONBOARD_LED_ACTIVE_LOW ? LOW : HIGH);
}

void printApStatus()
{
  Serial.print(F("AP mode="));
  Serial.print((WiFi.getMode() & WIFI_AP) ? F("on") : F("off"));
  Serial.print(F(", ssid='"));
  Serial.print(AP_SSID);
  Serial.print(F("', ip="));
  Serial.print(WiFi.softAPIP());
  Serial.print(F(", channel="));
  Serial.print(WiFi.channel());
  Serial.print(F(", clients="));
  Serial.println(WiFi.softAPgetStationNum());
}

String buildStatusJson()
{
  String json;
  json.reserve(256);
  json += F("{\"ssid\":\"");
  json += AP_SSID;
  json += F("\",\"ip\":\"");
  json += WiFi.softAPIP().toString();
  json += F("\",\"channel\":");
  json += WiFi.channel();
  json += F(",\"clients\":");
  json += WiFi.softAPgetStationNum();
  json += F(",\"mac\":\"");
  json += WiFi.softAPmacAddress();
  json += F("\"}");
  return json;
}

void startWebServer()
{
  // Debug route: list LittleFS contents
  webServer.on("/fs", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = F("{\"files\":[");
    Dir dir = LittleFS.openDir("/");
    bool first = true;
    while (dir.next())
    {
      if (!first) json += ",";
      json += F("{\"name\":\"");
      json += dir.fileName();
      json += F("\",\"size\":");
      json += dir.fileSize();
      json += F("}");
      first = false;
    }
    json += F("]}");
    request->send(200, "application/json", json);
  });

  // Serve static files from LittleFS
  webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    const String status = buildStatusJson();
    request->send(200, "application/json", status);
  });

  webEvents.onConnect([](AsyncEventSourceClient *client) {
    client->send("connected", nullptr, millis());
  });

  webServer.addHandler(&webEvents);

  webServer.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "application/json", "{\"error\":\"not found\"}");
  });

  webServer.begin();
  Serial.println(F("Web server started on port 80"));
}

void streamApStatus()
{
  static unsigned long lastStreamAtMs = 0;
  const unsigned long nowMs = millis();
  const unsigned long STREAM_INTERVAL_MS = 2000;

  if (nowMs - lastStreamAtMs >= STREAM_INTERVAL_MS)
  {
    lastStreamAtMs = nowMs;
    const String status = buildStatusJson();
    webEvents.send(status.c_str(), "status", nowMs);
  }
}

void setup()
{
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  setFlap(false);

  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("ESP8266 SoftAP with Web Dashboard"));
  Serial.println(F("Relay pin D7 ready (default LOW), LED mirrors flap state"));

  if (!LittleFS.begin())
  {
    Serial.println(F("LittleFS mount failed!"));
    Serial.println(F("Run: pio run -t uploadfs"));
  }
  else
  {
    Serial.println(F("LittleFS mounted successfully"));
    
    // Debug: list files
    Serial.println(F("LittleFS contents:"));
    Dir dir = LittleFS.openDir("/");
    while (dir.next())
    {
      Serial.print(F("  "));
      Serial.print(dir.fileName());
      Serial.print(F(" ("));
      Serial.print(dir.fileSize());
      Serial.println(F(" bytes)"));
    }
  }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);

  const bool started = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CLIENTS);

  Serial.print(F("softAP start result="));
  Serial.println(started ? F("ok") : F("failed"));
  Serial.print(F("AP MAC="));
  Serial.println(WiFi.softAPmacAddress());

  printApStatus();
  WiFi.printDiag(Serial);

  startWebServer();

  Serial.print(F("Web URL: http://"));
  Serial.println(WiFi.softAPIP());
}

void loop()
{
  const unsigned long nowMs = millis();
  if (nowMs - lastStatusAtMs >= STATUS_INTERVAL_MS)
  {
    lastStatusAtMs = nowMs;
    printApStatus();
  }

  streamApStatus();
}
