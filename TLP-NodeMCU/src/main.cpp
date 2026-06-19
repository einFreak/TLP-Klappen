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
const uint8_t SENSOR_PIN = A0;
const uint8_t BASELINE_FILTER_SHIFT = 4; // 1/16 IIR baseline smoothing
const uint8_t ENVELOPE_DECAY_SHIFT = 2;  // Faster decay = smaller value
const unsigned long SENSOR_SAMPLE_INTERVAL_MS = 50; // Keeps ADC from starving WiFi DHCP on ESP8266
const unsigned long MANUAL_OVERRIDE_MS = 10000;
const int HIT_THRESHOLD = 60;
const unsigned long TELEPLOT_INTERVAL_MS = 50;
const unsigned long DEBUG_INTERVAL_MS = 200;

// Stepwise activation switches for regression isolation.
const bool FEATURE_SENSOR_PIPELINE = true;
const bool FEATURE_SENSOR_JSON = true;
const bool FEATURE_SENSOR_SERIAL_LOG = true;

bool debugStreamingEnabled = true;
bool teleplotStreamingEnabled = false;

unsigned long lastDebugAtMs = 0;
unsigned long lastTeleplotAtMs = 0;

unsigned long lastStatusAtMs = 0;
const unsigned long STATUS_INTERVAL_MS = 3000;
bool flapIsUp = false;
bool manualOverrideEnabled = false;
bool hitLatched = false;
int hitThreshold = HIT_THRESHOLD;
unsigned long manualOverrideUntilMs = 0;
long baselineQ8 = 0;
int envelope = 0;
int lastRaw = 0;
int lastBaseline = 0;
int lastCentered = 0;
unsigned long lastSensorSampleAtMs = 0;

AsyncWebServer webServer(80);
AsyncEventSource webEvents("/events");

WiFiEventHandler wifiApStationConnectedHandler;
WiFiEventHandler wifiApStationDisconnectedHandler;

enum class ControlAction
{
  FlapUp,
  FlapDown,
  FlapToggle,
  AutoMode,
  ResetLatch
};

bool setThresholdValue(int value, bool printFeedback)
{
  if (value < 0 || value > 1023)
  {
    if (printFeedback)
    {
      Serial.println(F("CMD threshold invalid (expected 0..1023)"));
    }
    return false;
  }
  hitThreshold = value;
  if (printFeedback)
  {
    Serial.print(F("CMD threshold="));
    Serial.println(hitThreshold);
  }
  return true;
}

void setFlap(bool moveUp)
{
  flapIsUp = moveUp;
  digitalWrite(RELAY_PIN, moveUp == RELAY_ACTIVE_HIGH ? HIGH : LOW);
  digitalWrite(ONBOARD_LED_PIN, moveUp == ONBOARD_LED_ACTIVE_LOW ? LOW : HIGH);
}

void applyControlAction(ControlAction action, bool printFeedback)
{
  const unsigned long nowMs = millis();

  if (action == ControlAction::FlapUp)
  {
    setFlap(true);
    manualOverrideEnabled = true;
    manualOverrideUntilMs = nowMs + MANUAL_OVERRIDE_MS;
    if (printFeedback)
    {
      Serial.print(F("CMD flap=up, mode=manual for ms="));
      Serial.println(MANUAL_OVERRIDE_MS);
    }
  }
  else if (action == ControlAction::FlapDown)
  {
    setFlap(false);
    manualOverrideEnabled = true;
    manualOverrideUntilMs = nowMs + MANUAL_OVERRIDE_MS;
    if (printFeedback)
    {
      Serial.print(F("CMD flap=down, mode=manual for ms="));
      Serial.println(MANUAL_OVERRIDE_MS);
    }
  }
  else if (action == ControlAction::FlapToggle)
  {
    setFlap(!flapIsUp);
    manualOverrideEnabled = true;
    manualOverrideUntilMs = nowMs + MANUAL_OVERRIDE_MS;
    if (printFeedback)
    {
      Serial.print(F("CMD flap="));
      Serial.print(flapIsUp ? F("up") : F("down"));
      Serial.print(F(", mode=manual for ms="));
      Serial.println(MANUAL_OVERRIDE_MS);
    }
  }
  else if (action == ControlAction::AutoMode)
  {
    manualOverrideEnabled = false;
    manualOverrideUntilMs = 0;
    if (printFeedback)
    {
      Serial.println(F("CMD mode=auto"));
    }
  }
  else if (action == ControlAction::ResetLatch)
  {
    hitLatched = false;
    setFlap(false);
    if (printFeedback)
    {
      Serial.println(F("CMD reset (latched=0, flap=down)"));
    }
  }
}

void printTeleplot(unsigned long nowMs)
{
  Serial.print(F(">sensor_raw:"));
  Serial.print(nowMs); Serial.print(':'); Serial.println(lastRaw);
  Serial.print(F(">sensor_envelope:"));
  Serial.print(nowMs); Serial.print(':'); Serial.println(envelope);
  Serial.print(F(">flap_state:"));
  Serial.print(nowMs); Serial.print(':'); Serial.println(flapIsUp ? 1 : 0);
  Serial.print(F(">hitThreshold:"));
  Serial.print(nowMs); Serial.print(':'); Serial.println(hitThreshold);
}

void printSample(unsigned long nowMs)
{
  Serial.print(F("SAMPLE ms=")); Serial.print(nowMs);
  Serial.print(F(" raw="));      Serial.print(lastRaw);
  Serial.print(F(" baseline=")); Serial.print(lastBaseline);
  Serial.print(F(" centered=")); Serial.print(lastCentered);
  Serial.print(F(" envelope=")); Serial.print(envelope);
  Serial.print(F(" thr="));      Serial.print(hitThreshold);
  Serial.print(F(" latched="));  Serial.print(hitLatched ? 1 : 0);
  Serial.print(F(" mode="));     Serial.print(manualOverrideEnabled ? F("manual") : F("auto"));
  Serial.print(F(" flap="));     Serial.println(flapIsUp ? 1 : 0);
}

void printHelp()
{
  Serial.println(F("Commands:"));
  Serial.println(F("  u = flap up"));
  Serial.println(F("  l = flap down"));
  Serial.println(F("  t = toggle flap"));
  Serial.println(F("  a = auto mode"));
  Serial.println(F("  r = reset latch"));
  Serial.println(F("  d = toggle debug stream"));
  Serial.println(F("  p = toggle teleplot stream"));
  Serial.println(F("  s=<0..1023> = set threshold"));
  Serial.println(F("  ? = this help"));
}

void processSerialCommand(const char *command)
{
  if (command[0] == '\0') return;

  if ((command[0] == 's' || command[0] == 'S') && command[1] == '=')
  {
    const long parsed = strtol(command + 2, nullptr, 10);
    setThresholdValue(static_cast<int>(parsed), true);
    return;
  }

  const char single = command[0];
  if      (single == 'u' || single == 'U') applyControlAction(ControlAction::FlapUp,     true);
  else if (single == 'l' || single == 'L') applyControlAction(ControlAction::FlapDown,   true);
  else if (single == 't' || single == 'T') applyControlAction(ControlAction::FlapToggle, true);
  else if (single == 'a' || single == 'A') applyControlAction(ControlAction::AutoMode,   true);
  else if (single == 'r' || single == 'R') applyControlAction(ControlAction::ResetLatch, true);
  else if (single == 'd' || single == 'D')
  {
    debugStreamingEnabled = !debugStreamingEnabled;
    Serial.print(F("CMD debug_stream="));
    Serial.println(debugStreamingEnabled ? F("on") : F("off"));
  }
  else if (single == 'p' || single == 'P')
  {
    teleplotStreamingEnabled = !teleplotStreamingEnabled;
    Serial.print(F("CMD teleplot_stream="));
    Serial.println(teleplotStreamingEnabled ? F("on") : F("off"));
  }
  else if (single == '?') printHelp();
}

void handleSerial()
{
  static char buf[32];
  static uint8_t len = 0;

  while (Serial.available() > 0)
  {
    const char c = static_cast<char>(Serial.read());

    // One-letter commands fire immediately, before buffering.
    if (len == 0 && (c=='u'||c=='U'||c=='l'||c=='L'||c=='t'||c=='T'||
                     c=='a'||c=='A'||c=='r'||c=='R'||c=='d'||c=='D'||
                     c=='p'||c=='P'||c=='?'))
    {
      buf[0] = c; buf[1] = '\0'; processSerialCommand(buf);
      continue;
    }

    if (c == '\n' || c == '\r')
    {
      if (len > 0) { buf[len] = '\0'; processSerialCommand(buf); len = 0; }
      continue;
    }

    if (len < sizeof(buf) - 1) buf[len++] = c;
  }
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
  json.reserve(340);
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
  json += F("\"");
  json += F(",\"flap\":");
  json += flapIsUp ? 1 : 0;
  json += F(",\"latched\":");
  json += hitLatched ? 1 : 0;
  json += F(",\"mode\":\"");
  json += manualOverrideEnabled ? F("manual") : F("auto");
  json += F("\"");
  json += F(",\"threshold\":");
  json += hitThreshold;
  if (FEATURE_SENSOR_JSON)
  {
    json += F(",\"raw\":");
    json += lastRaw;
    json += F(",\"baseline\":");
    json += lastBaseline;
    json += F(",\"centered\":");
    json += lastCentered;
    json += F(",\"envelope\":");
    json += envelope;
  }
  json += F("}");
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

  // API routes MUST be registered before serveStatic, otherwise the
  // catch-all static handler intercepts them and returns 404.
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildStatusJson());
  });

  webServer.on("/api/flap", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action;
    if      (request->hasParam("action", true)) action = request->getParam("action", true)->value();
    else if (request->hasParam("action"))       action = request->getParam("action")->value();
    action.toLowerCase();

    if      (action == "up")     applyControlAction(ControlAction::FlapUp,     true);
    else if (action == "down")   applyControlAction(ControlAction::FlapDown,   true);
    else if (action == "toggle") applyControlAction(ControlAction::FlapToggle, true);
    else if (action == "auto")   applyControlAction(ControlAction::AutoMode,   true);
    else if (action == "reset")  applyControlAction(ControlAction::ResetLatch, true);
    else { request->send(400, "application/json", "{\"error\":\"invalid action\"}"); return; }
    request->send(200, "application/json", buildStatusJson());
  });

  webServer.on("/api/threshold", HTTP_POST, [](AsyncWebServerRequest *request) {
    String val;
    if      (request->hasParam("value", true)) val = request->getParam("value", true)->value();
    else if (request->hasParam("value"))       val = request->getParam("value")->value();
    if (val.length() == 0) { request->send(400, "application/json", "{\"error\":\"missing value\"}"); return; }
    const long parsed = strtol(val.c_str(), nullptr, 10);
    if (!setThresholdValue(static_cast<int>(parsed), true))
    { request->send(400, "application/json", "{\"error\":\"value out of range\"}"); return; }
    request->send(200, "application/json", buildStatusJson());
  });

  webEvents.onConnect([](AsyncEventSourceClient *client) {
    client->send("connected", nullptr, millis());
  });

  webServer.addHandler(&webEvents);

  // Static files last — acts as catch-all for everything not matched above.
  webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

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
  Serial.print(F("Feature sensor pipeline="));
  Serial.println(FEATURE_SENSOR_PIPELINE ? F("on") : F("off"));

  if (FEATURE_SENSOR_PIPELINE)
  {
    baselineQ8 = static_cast<long>(analogRead(SENSOR_PIN)) << 8;
  }

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

  wifiApStationConnectedHandler = WiFi.onSoftAPModeStationConnected(
    [](const WiFiEventSoftAPModeStationConnected &e) {
      Serial.print(F("AP: station connected aid="));
      Serial.printf("%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
        e.aid, e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
    });

  wifiApStationDisconnectedHandler = WiFi.onSoftAPModeStationDisconnected(
    [](const WiFiEventSoftAPModeStationDisconnected &e) {
      Serial.print(F("AP: station disconnected aid="));
      Serial.println(e.aid);
    });

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
  printHelp();
}

void loop()
{
  const unsigned long nowMs = millis();

  handleSerial();

  // Manual override timeout -> revert to auto.
  if (manualOverrideEnabled && nowMs >= manualOverrideUntilMs)
  {
    manualOverrideEnabled = false;
    manualOverrideUntilMs = 0;
    Serial.println(F("MODE auto (manual override timeout)"));
  }

  if (FEATURE_SENSOR_PIPELINE && (nowMs - lastSensorSampleAtMs >= SENSOR_SAMPLE_INTERVAL_MS))
  {
    lastSensorSampleAtMs = nowMs;

    // Sensor pipeline: raw ADC -> moving baseline -> centered -> envelope.
    lastRaw = analogRead(SENSOR_PIN);
    baselineQ8 += ((static_cast<long>(lastRaw) << 8) - baselineQ8) >> BASELINE_FILTER_SHIFT;
    lastBaseline = static_cast<int>(baselineQ8 >> 8);
    lastCentered = lastRaw - lastBaseline;

    const int magnitude = abs(lastCentered);
    if (magnitude > envelope)
    {
      envelope = magnitude;
    }
    else if (envelope > 0)
    {
      envelope -= max(1, envelope >> ENVELOPE_DECAY_SHIFT);
    }
  }

  if (nowMs - lastStatusAtMs >= STATUS_INTERVAL_MS)
  {
    lastStatusAtMs = nowMs;
    printApStatus();
    if (FEATURE_SENSOR_SERIAL_LOG)
    {
      Serial.print(F("SENSOR raw="));
      Serial.print(lastRaw);
      Serial.print(F(" baseline="));
      Serial.print(lastBaseline);
      Serial.print(F(" centered="));
      Serial.print(lastCentered);
      Serial.print(F(" envelope="));
      Serial.println(envelope);
    }
  }

  if (debugStreamingEnabled && (nowMs - lastDebugAtMs >= DEBUG_INTERVAL_MS))
  {
    lastDebugAtMs = nowMs;
    printSample(nowMs);
  }

  if (teleplotStreamingEnabled && (nowMs - lastTeleplotAtMs >= TELEPLOT_INTERVAL_MS))
  {
    lastTeleplotAtMs = nowMs;
    printTeleplot(nowMs);
  }

  // Automatic hit trigger: envelope over threshold -> flap up + latch.
  // Only active when sensor pipeline is running and no manual override is active.
  if (FEATURE_SENSOR_PIPELINE && !manualOverrideEnabled && !hitLatched && envelope >= hitThreshold)
  {
    hitLatched = true;
    setFlap(true);
    Serial.print(F("HIT ms="));
    Serial.print(nowMs);
    Serial.print(F(" raw="));
    Serial.print(lastRaw);
    Serial.print(F(" centered="));
    Serial.print(lastCentered);
    Serial.print(F(" envelope="));
    Serial.print(envelope);
    Serial.print(F(" threshold="));
    Serial.println(hitThreshold);
  }

  streamApStatus();

  // Keep WiFi background tasks responsive even with active sensor processing.
  yield();
}
