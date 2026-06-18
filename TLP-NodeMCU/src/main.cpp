/*
  TLP-Klappen - Phase A single node bring-up (ESP8266)

  Wiring assumptions for first hardware tests (NodeMCU v2):
  - Piezo / vibration sensor analog output -> A0
  - Sensor VCC/GND -> board VCC/GND
  - Relay control input -> D7 (GPIO13)

  Safety note:
  - Do NOT drive a bare relay coil directly from the ESP8266 pin.
  - Use a relay module with transistor/driver stage, or add an external
    transistor/MOSFET plus flyback diode across the coil.
  - Share ground between the ESP8266 and the relay driver stage.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

const char WIFI_STA_SSID[] = "Connecto Patronum24";
const char WIFI_STA_PASSWORD[] = "0h4na-M34ns-F4m1ly";
// const char WIFI_AP_SSID[] = "TLP-Klappen";
// const char WIFI_AP_PASSWORD[] = "tlp12345";

const uint8_t SENSOR_PIN = A0;
const uint8_t RELAY_PIN = D7;
const bool RELAY_ACTIVE_HIGH = true;
const uint8_t ONBOARD_LED_PIN = LED_BUILTIN;
const bool ONBOARD_LED_ACTIVE_LOW = true;

const int HIT_THRESHOLD = 60;                // Envelope threshold to move flap up
const int RELEASE_THRESHOLD = 35;            // Lower threshold to move flap down
const unsigned long DEBUG_INTERVAL_MS = 50; // Periodic debug output
const unsigned long TELEPLOT_INTERVAL_MS = 50; // Periodic TelePlot output
const unsigned long MANUAL_OVERRIDE_MS = 10000; // Hold manual control after serial flap command
const unsigned long WEB_STREAM_INTERVAL_MS = 100; // Dashboard SSE interval
const unsigned long WIFI_STATUS_INTERVAL_MS = 5000; // Periodic WiFi status output
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000; // Retry STA association while disconnected
const unsigned long WIFI_SCAN_DEBUG_INTERVAL_MS = 30000; // Limit active scan spam while debugging

const uint8_t BASELINE_FILTER_SHIFT = 4; // 1/16 IIR baseline smoothing
const uint8_t ENVELOPE_DECAY_SHIFT = 2;  // Faster decay = smaller value

long baselineQ8 = 0;
int envelope = 0;
int lastRaw = 0;
int lastBaseline = 0;
int lastCentered = 0;
int hitThreshold = HIT_THRESHOLD;

bool flapIsUp = false;
bool debugStreamingEnabled = true;
bool teleplotStreamingEnabled = true;
bool manualOverrideEnabled = false;
bool hitLatched = false;

unsigned long lastDebugAtMs = 0;
unsigned long lastTeleplotAtMs = 0;
unsigned long manualOverrideUntilMs = 0;
unsigned long lastWebStreamAtMs = 0;
unsigned long lastWifiStatusAtMs = 0;
unsigned long lastWifiReconnectAtMs = 0;
unsigned long lastWifiScanAtMs = 0;
wl_status_t previousWifiStatus = WL_IDLE_STATUS;
bool wifiTargetCandidateValid = false;
uint8_t wifiTargetBssid[6] = {0};
int wifiTargetChannel = 0;
int wifiTargetRssi = -127;

WiFiEventHandler wifiConnectedHandler;
WiFiEventHandler wifiDisconnectedHandler;
WiFiEventHandler wifiGotIpHandler;

AsyncWebServer webServer(80);
AsyncEventSource webEvents("/events");

enum class ControlAction
{
  FlapUp,
  FlapDown,
  FlapToggle,
  AutoMode,
  ResetLatch
};

void setFlap(bool moveUp)
{
  flapIsUp = moveUp;
  digitalWrite(RELAY_PIN, moveUp == RELAY_ACTIVE_HIGH ? HIGH : LOW);
  digitalWrite(ONBOARD_LED_PIN, moveUp == ONBOARD_LED_ACTIVE_LOW ? LOW : HIGH);
}

String buildStatusJson(unsigned long nowMs)
{
  String json;
  json.reserve(220);
  json += F("{\"ms\":");
  json += nowMs;
  json += F(",\"raw\":");
  json += lastRaw;
  json += F(",\"baseline\":");
  json += lastBaseline;
  json += F(",\"centered\":");
  json += lastCentered;
  json += F(",\"envelope\":");
  json += envelope;
  json += F(",\"threshold\":");
  json += hitThreshold;
  json += F(",\"flap\":");
  json += flapIsUp ? 1 : 0;
  json += F(",\"latched\":");
  json += hitLatched ? 1 : 0;
  json += F(",\"mode\":\"");
  json += manualOverrideEnabled ? F("manual") : F("auto");
  json += F("\",\"staConnected\":");
  json += WiFi.status() == WL_CONNECTED ? 1 : 0;
  json += F("}");
  return json;
}

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
      Serial.print(F("CMD flap=up (HIGH), mode=manual for ms="));
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
      Serial.print(F("CMD flap=down (LOW), mode=manual for ms="));
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
      Serial.print(flapIsUp ? F("up (HIGH)") : F("down (LOW)"));
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

const String &requestParamValue(AsyncWebServerRequest *request, const char *name, String &storage)
{
  if (request->hasParam(name, true))
  {
    storage = request->getParam(name, true)->value();
    return storage;
  }
  if (request->hasParam(name))
  {
    storage = request->getParam(name)->value();
    return storage;
  }
  storage = "";
  return storage;
}

void sendStatusJson(AsyncWebServerRequest *request)
{
  const String status = buildStatusJson(millis());
  request->send(200, "application/json", status);
}

void handleFlapApi(AsyncWebServerRequest *request)
{
  String action;
  requestParamValue(request, "action", action);
  action.toLowerCase();

  if (action == "up")
  {
    applyControlAction(ControlAction::FlapUp, true);
  }
  else if (action == "down")
  {
    applyControlAction(ControlAction::FlapDown, true);
  }
  else if (action == "toggle")
  {
    applyControlAction(ControlAction::FlapToggle, true);
  }
  else if (action == "auto")
  {
    applyControlAction(ControlAction::AutoMode, true);
  }
  else if (action == "reset")
  {
    applyControlAction(ControlAction::ResetLatch, true);
  }
  else
  {
    request->send(400, "application/json", "{\"error\":\"invalid action\"}");
    return;
  }

  sendStatusJson(request);
}

void handleThresholdApi(AsyncWebServerRequest *request)
{
  String valueText;
  requestParamValue(request, "value", valueText);
  if (valueText.length() == 0)
  {
    request->send(400, "application/json", "{\"error\":\"missing value\"}");
    return;
  }

  const long parsed = strtol(valueText.c_str(), nullptr, 10);
  if (!setThresholdValue(static_cast<int>(parsed), true))
  {
    request->send(400, "application/json", "{\"error\":\"value out of range\"}");
    return;
  }

  sendStatusJson(request);
}

void startWifi()
{
  WiFi.persistent(false);
  // AP mode disabled for troubleshooting STA association.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setAutoConnect(true);

  wifiConnectedHandler = WiFi.onStationModeConnected([](const WiFiEventStationModeConnected &event)
                                                     {
                                                       Serial.print(F("WIFI event: connected ssid='"));
                                                       Serial.print(event.ssid);
                                                       Serial.print(F("' channel="));
                                                       Serial.println(event.channel);
                                                     });

  wifiDisconnectedHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &event)
                                                           {
                                                             Serial.print(F("WIFI event: disconnected reason="));
                                                             Serial.println(static_cast<int>(event.reason));
                                                           });

  wifiGotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &event)
                                             {
                                               Serial.print(F("WIFI event: got_ip="));
                                               Serial.print(event.ip);
                                               Serial.print(F(" gw="));
                                               Serial.print(event.gw);
                                               Serial.print(F(" mask="));
                                               Serial.println(event.mask);
                                             });

  if (strlen(WIFI_STA_SSID) > 0 && strlen(WIFI_STA_PASSWORD) > 0)
  {
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);
    Serial.print(F("WIFI STA connecting to ssid='"));
    Serial.print(WIFI_STA_SSID);
    Serial.println(F("'"));
  }
  else
  {
    Serial.println(F("WIFI STA skipped (set WIFI_STA_SSID/PASSWORD in code)"));
  }
}

const __FlashStringHelper *wifiStatusText(wl_status_t status)
{
  switch (status)
  {
  case WL_IDLE_STATUS:
    return F("idle");
  case WL_NO_SSID_AVAIL:
    return F("no_ssid");
  case WL_SCAN_COMPLETED:
    return F("scan_completed");
  case WL_CONNECTED:
    return F("connected");
  case WL_CONNECT_FAILED:
    return F("connect_failed");
  case WL_CONNECTION_LOST:
    return F("connection_lost");
  case WL_DISCONNECTED:
    return F("disconnected");
  default:
    return F("unknown");
  }
}

const __FlashStringHelper *wifiEncryptionText(uint8_t encryptionType)
{
  switch (encryptionType)
  {
  case ENC_TYPE_NONE:
    return F("open");
  case ENC_TYPE_WEP:
    return F("WEP");
  case ENC_TYPE_TKIP:
    return F("WPA/TKIP");
  case ENC_TYPE_CCMP:
    return F("WPA2/CCMP");
  case ENC_TYPE_AUTO:
    return F("AUTO");
  default:
    return F("unknown");
  }
}

void debugWifiScan(unsigned long nowMs)
{
  if (nowMs - lastWifiScanAtMs < WIFI_SCAN_DEBUG_INTERVAL_MS)
  {
    return;
  }

  lastWifiScanAtMs = nowMs;

  Serial.println(F("WIFI scan: start"));
  const int found = WiFi.scanNetworks(false, true);
  Serial.print(F("WIFI scan: found="));
  Serial.println(found);

  if (found <= 0)
  {
    wifiTargetCandidateValid = false;
    return;
  }

  bool targetFound = false;
  int bestTargetIndex = -1;
  int bestTargetRssi = -127;
  for (int i = 0; i < found; ++i)
  {
    const String ssid = WiFi.SSID(i);
    Serial.print(F("  ["));
    Serial.print(i);
    Serial.print(F("] ssid='"));
    Serial.print(ssid);
    Serial.print(F("' rssi="));
    Serial.print(WiFi.RSSI(i));
    Serial.print(F(" dBm ch="));
    Serial.print(WiFi.channel(i));
    Serial.print(F(" enc="));
    Serial.println(wifiEncryptionText(WiFi.encryptionType(i)));

    if (ssid == WIFI_STA_SSID)
    {
      targetFound = true;
      if (WiFi.RSSI(i) > bestTargetRssi)
      {
        bestTargetRssi = WiFi.RSSI(i);
        bestTargetIndex = i;
      }
    }
  }

  Serial.print(F("WIFI scan: target_ssid_visible="));
  Serial.println(targetFound ? F("yes") : F("no"));

  if (bestTargetIndex >= 0)
  {
    wifiTargetCandidateValid = true;
    wifiTargetChannel = WiFi.channel(bestTargetIndex);
    wifiTargetRssi = WiFi.RSSI(bestTargetIndex);
    memcpy(wifiTargetBssid, WiFi.BSSID(bestTargetIndex), sizeof(wifiTargetBssid));

    Serial.print(F("WIFI scan: target_best_rssi="));
    Serial.print(wifiTargetRssi);
    Serial.print(F(" dBm channel="));
    Serial.print(wifiTargetChannel);
    Serial.print(F(" bssid="));
    Serial.println(WiFi.BSSIDstr(bestTargetIndex));
  }
  else
  {
    wifiTargetCandidateValid = false;
  }
}

void attemptWifiReconnect(unsigned long nowMs)
{
  if (strlen(WIFI_STA_SSID) == 0 || strlen(WIFI_STA_PASSWORD) == 0)
  {
    return;
  }

  if (nowMs - lastWifiReconnectAtMs < WIFI_RECONNECT_INTERVAL_MS)
  {
    return;
  }

  lastWifiReconnectAtMs = nowMs;

  Serial.print(F("WIFI reconnect attempt: ssid='"));
  Serial.print(WIFI_STA_SSID);
  Serial.print(F("', status="));
  const wl_status_t status = WiFi.status();
  Serial.print(wifiStatusText(status));
  Serial.print(F(" ("));
  Serial.print(static_cast<int>(status));
  Serial.println(F(")"));

  if (status == WL_NO_SSID_AVAIL)
  {
    debugWifiScan(nowMs);
  }

  WiFi.disconnect(false);

  if (wifiTargetCandidateValid)
  {
    Serial.print(F("WIFI reconnect: using target BSSID on channel "));
    Serial.print(wifiTargetChannel);
    Serial.print(F(", rssi="));
    Serial.print(wifiTargetRssi);
    Serial.println(F(" dBm"));
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD, wifiTargetChannel, wifiTargetBssid, true);
  }
  else
  {
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);
  }

  WiFi.printDiag(Serial);
}

void startWebServer()
{
  webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  webServer.on("/status", HTTP_GET, [](AsyncWebServerRequest *request)
               { sendStatusJson(request); });

  webServer.on("/api/flap", HTTP_POST, [](AsyncWebServerRequest *request)
               { handleFlapApi(request); });

  webServer.on("/api/threshold", HTTP_POST, [](AsyncWebServerRequest *request)
               { handleThresholdApi(request); });

  webEvents.onConnect([](AsyncEventSourceClient *client)
                      {
                        client->send("connected", "hello", millis());
                        const String status = buildStatusJson(millis());
                        client->send(status.c_str(), "state", millis());
                      });

  webServer.addHandler(&webEvents);
  webServer.onNotFound([](AsyncWebServerRequest *request)
                       { request->send(404, "application/json", "{\"error\":\"not found\"}"); });
  webServer.begin();
  Serial.println(F("WEB server started: static files from LittleFS"));
}

void streamWebStatus(unsigned long nowMs)
{
  if (nowMs - lastWebStreamAtMs < WEB_STREAM_INTERVAL_MS)
  {
    return;
  }

  lastWebStreamAtMs = nowMs;
  const String status = buildStatusJson(nowMs);
  webEvents.send(status.c_str(), "state", nowMs);
}

void printWifiStatus(unsigned long nowMs)
{
  if (nowMs - lastWifiStatusAtMs < WIFI_STATUS_INTERVAL_MS)
  {
    return;
  }

  lastWifiStatusAtMs = nowMs;
  const wl_status_t status = WiFi.status();
  if (status != previousWifiStatus)
  {
    Serial.print(F("WIFI status changed: "));
    Serial.print(wifiStatusText(previousWifiStatus));
    Serial.print(F(" -> "));
    Serial.println(wifiStatusText(status));
    previousWifiStatus = status;
  }

  Serial.print(F("WIFI status: sta="));
  Serial.print(wifiStatusText(status));
  Serial.print(F(" ("));
  Serial.print(static_cast<int>(status));
  Serial.print(F(")"));
  if (status == WL_CONNECTED)
  {
    Serial.print(F(", sta_ip="));
    Serial.print(WiFi.localIP());
    Serial.print(F(", rssi="));
    Serial.print(WiFi.RSSI());
    Serial.print(F(" dBm"));
    Serial.print(F(", channel="));
    Serial.print(WiFi.channel());
    Serial.print(F(", bssid="));
    Serial.print(WiFi.BSSIDstr());
  }
  Serial.println();
}

void printWebUrls()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print(F("WEB URL (STA): http://"));
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println(F("WEB URL (STA): not connected yet"));
  }
}

void printHelp()
{
  Serial.println(F("Commands:"));
  Serial.println(F("  u = move flap up (HIGH)"));
  Serial.println(F("  l = move flap down (LOW)"));
  Serial.println(F("  t = toggle flap state"));
  Serial.println(F("  a = return to automatic hit/release mode now"));
  Serial.println(F("  r = reset latched hit and flap down"));
  Serial.println(F("  d = toggle periodic debug stream"));
  Serial.println(F("  p = toggle TelePlot stream"));
  Serial.println(F("  w = print web dashboard URLs"));
  Serial.println(F("  s=<0..1023> = set hit threshold"));
  Serial.println(F("  ? = print this help"));
}

void printTeleplot(unsigned long nowMs)
{
  // TelePlot serial format on Serial ports requires a leading '>' prefix.
  // Frame format: name:timestamp:value[§unit|flags]
  Serial.print(F(">sensor_raw:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(lastRaw);

  Serial.print(F(">sensor_envelope:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(envelope);

  Serial.print(F(">flap_state:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(flapIsUp ? 1 : 0);

  Serial.print(F(">hitThreshold:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(hitThreshold);
}

void printSample(unsigned long nowMs)
{
  Serial.print(F("SAMPLE ms="));
  Serial.print(nowMs);
  Serial.print(F(" raw="));
  Serial.print(lastRaw);
  Serial.print(F(" baseline="));
  Serial.print(lastBaseline);
  Serial.print(F(" centered="));
  Serial.print(lastCentered);
  Serial.print(F(" envelope="));
  Serial.print(envelope);
  Serial.print(F(" threshold="));
  Serial.print(hitThreshold);
  Serial.print(F(" latched="));
  Serial.print(hitLatched ? 1 : 0);
  Serial.print(F(" mode="));
  Serial.print(manualOverrideEnabled ? F("manual") : F("auto"));
  Serial.print(F(" flap="));
  Serial.println(flapIsUp ? 1 : 0);
}

void processSerialCommand(const char *command)
{
  if (command[0] == '\0')
  {
    return;
  }

  if ((command[0] == 's' || command[0] == 'S') && command[1] == '=')
  {
    const long parsed = strtol(command + 2, nullptr, 10);
    setThresholdValue(static_cast<int>(parsed), true);
    return;
  }

  const char single = command[0];
  if (single == 'u' || single == 'U')
  {
    applyControlAction(ControlAction::FlapUp, true);
  }
  else if (single == 'l' || single == 'L')
  {
    applyControlAction(ControlAction::FlapDown, true);
  }
  else if (single == 't' || single == 'T')
  {
    applyControlAction(ControlAction::FlapToggle, true);
  }
  else if (single == 'a' || single == 'A')
  {
    applyControlAction(ControlAction::AutoMode, true);
  }
  else if (single == 'r' || single == 'R')
  {
    applyControlAction(ControlAction::ResetLatch, true);
  }
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
  else if (single == 'w' || single == 'W')
  {
    printWebUrls();
  }
  else if (single == '?')
  {
    printHelp();
  }
}

void handleSerial()
{
  static char commandBuffer[32];
  static uint8_t commandLength = 0;

  while (Serial.available() > 0)
  {
    const char c = static_cast<char>(Serial.read());

    if (c == '\n' || c == '\r')
    {
      if (commandLength > 0)
      {
        commandBuffer[commandLength] = '\0';
        processSerialCommand(commandBuffer);
        commandLength = 0;
      }
      continue;
    }

    if (commandLength < sizeof(commandBuffer) - 1)
    {
      commandBuffer[commandLength++] = c;
    }

    // Backward compatibility for one-letter commands sent without line ending.
    if (commandLength == 1 && (c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 't' || c == 'T' || c == 'a' || c == 'A' || c == 'r' || c == 'R' || c == 'd' || c == 'D' || c == 'p' || c == 'P' || c == 'w' || c == 'W' || c == '?'))
    {
      commandBuffer[1] = '\0';
      processSerialCommand(commandBuffer);
      commandLength = 0;
    }
  }
}

void setup()
{
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  setFlap(false);

  Serial.begin(115200);

  baselineQ8 = static_cast<long>(analogRead(SENSOR_PIN)) << 8;

  Serial.println();
  Serial.println(F("TLP-Klappen Phase A single-node test (ESP8266)"));
  Serial.println(F("Board: NodeMCU / ESP8266"));
  Serial.println(F("Sensor: analog vibration input on A0"));
  Serial.println(F("Relay: direct level control on D7 (GPIO13)"));
  Serial.println(F("HIGH = flap up, LOW = flap down"));
  Serial.println(F("Web dashboard: STA mode with SSE stream"));
  Serial.println(F("Use serial commands: u/l/t (manual), a (auto), d = debug, p = teleplot, ? = help"));

  if (!LittleFS.begin())
  {
    Serial.println(F("LittleFS mount failed. Run: pio run -t uploadfs"));
  }

  startWifi();
  startWebServer();
  printWebUrls();

  printHelp();
}

void loop()
{
  // 1) Shared timestamp for this cycle.
  const unsigned long nowMs = millis();

  // 2) Handle incoming serial commands first to keep control responsive.
  handleSerial();

  // 3) Exit manual override automatically after timeout.
  if (manualOverrideEnabled && nowMs >= manualOverrideUntilMs)
  {
    manualOverrideEnabled = false;
    manualOverrideUntilMs = 0;
    Serial.println(F("MODE auto (manual override timeout)"));
  }

  // 4) Read sensor and update signal features.
  lastRaw = analogRead(SENSOR_PIN);

  // Slow baseline tracking (IIR): follows sensor drift, but not fast shocks/hits.
  baselineQ8 += ((static_cast<long>(lastRaw) << 8) - baselineQ8) >> BASELINE_FILTER_SHIFT;
  lastBaseline = static_cast<int>(baselineQ8 >> 8);
  // AC-like signal component around the moving baseline.
  lastCentered = lastRaw - lastBaseline;

  // Envelope follower: rise immediately on peaks, then decay smoothly over time.
  const int magnitude = abs(lastCentered);
  if (magnitude > envelope)
  {
    envelope = magnitude;
  }
  else if (envelope > 0)
  {
    envelope -= max(1, envelope >> ENVELOPE_DECAY_SHIFT);
  }

  // 5) Emit periodic diagnostics/telemetry.
  // if (debugStreamingEnabled && nowMs - lastDebugAtMs >= DEBUG_INTERVAL_MS)
  // {
  //   lastDebugAtMs = nowMs;
  //   printSample(nowMs);
  // }

  if (teleplotStreamingEnabled && nowMs - lastTeleplotAtMs >= TELEPLOT_INTERVAL_MS)
  {
    lastTeleplotAtMs = nowMs;
    printTeleplot(nowMs);
  }

  // 6) Push dashboard updates and periodic WiFi status.
  if (WiFi.status() != WL_CONNECTED)
  {
    attemptWifiReconnect(nowMs);
  }

  streamWebStatus(nowMs);
  printWifiStatus(nowMs);

  // // 7) Automatic trigger logic (disabled while manual override is active).
  // if (!manualOverrideEnabled && !hitLatched && envelope >= hitThreshold)
  // {
  //   setFlap(true);
  //   hitLatched = true;

  //   Serial.print(F("HIT ms="));
  //   Serial.print(nowMs);
  //   Serial.print(F(" raw="));
  //   Serial.print(lastRaw);
  //   Serial.print(F(" centered="));
  //   Serial.print(lastCentered);
  //   Serial.print(F(" envelope="));
  //   Serial.print(envelope);
  //   Serial.print(F(" threshold="));
  //   Serial.println(hitThreshold);
  // }
}
