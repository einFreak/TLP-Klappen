#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <mcp_can.h>

const char AP_SSID[] = "TLP-Klappen";
const char AP_PASSWORD[] = "tlp12345"; // WPA2, >= 8 chars
const uint8_t AP_CHANNEL = 1;
const bool AP_HIDDEN = false;
const uint8_t AP_MAX_CLIENTS = 4;
const bool RELAY_ACTIVE_HIGH = true;
const uint8_t ONBOARD_LED_PIN = LED_BUILTIN;
const bool ONBOARD_LED_ACTIVE_LOW = true;
const uint8_t SENSOR_PIN = A0;
const uint8_t MCP2515_CS_PIN = D1;
const uint8_t MCP2515_INT_PIN = D0;
const uint8_t BASELINE_FILTER_SHIFT = 4; // 1/16 IIR baseline smoothing
const uint8_t ENVELOPE_DECAY_SHIFT = 2;  // Faster decay = smaller value
const unsigned long SENSOR_SAMPLE_INTERVAL_MS = 50; // Keeps ADC from starving WiFi DHCP on ESP8266
const unsigned long MANUAL_OVERRIDE_MS = 10000;
const int HIT_THRESHOLD = 60;
const unsigned long TELEPLOT_INTERVAL_MS = 50;
const unsigned long DEBUG_INTERVAL_MS = 200;
const unsigned long CAN_STATUS_INTERVAL_MS = 5000;

bool debugStreamingEnabled = false;
bool teleplotStreamingEnabled = false;

unsigned long lastDebugAtMs = 0;
unsigned long lastTeleplotAtMs = 0;
unsigned long lastCanStatusAtMs = 0;

unsigned long lastStatusAtMs = 0;
const unsigned long STATUS_INTERVAL_MS = 3000;
bool flapIsUp = false;
bool manualOverrideEnabled = false;
bool hitLatched = false;
int hitThreshold = HIT_THRESHOLD;
unsigned long manualOverrideUntilMs = 0;
long baselineQ8 = 0;
int envelope = 0;
int lastTriggerRaw = -1;
int lastTriggerEnvelope = -1;
unsigned long lastTriggerAtMs = 0;
int lastRaw = 0;
int lastBaseline = 0;
int lastCentered = 0;
unsigned long lastSensorSampleAtMs = 0;

bool canControllerDetected = false;
bool canLoopbackPassed = false;
bool canNormalModeActive = false;
uint32_t canRxFrameCount = 0;
uint32_t canRxIgnoredCount = 0;

INT8U detectedCanClock = MCP_8MHZ;
MCP_CAN canController(MCP2515_CS_PIN);

AsyncWebServer webServer(80);
AsyncEventSource webEvents("/events");

WiFiEventHandler wifiApStationConnectedHandler;
WiFiEventHandler wifiApStationDisconnectedHandler;

const __FlashStringHelper *canClockToText(INT8U clockSel)
{
  if (clockSel == MCP_8MHZ) return F("8MHz");
  if (clockSel == MCP_16MHZ) return F("16MHz");
  if (clockSel == MCP_10MHZ) return F("10MHz");
  return F("unknown");
}

bool detectCanControllerClock()
{
  struct ClockCandidate
  {
    INT8U clock;
    const __FlashStringHelper *label;
  };

  const ClockCandidate candidates[] = {
    {MCP_8MHZ, F("8MHz")},
    {MCP_16MHZ, F("16MHz")},
    {MCP_10MHZ, F("10MHz")},
  };

  for (uint8_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
  {
    Serial.print(F("CAN init probe "));
    Serial.print(candidates[i].label);
    Serial.println(F(" ..."));

    const INT8U initResult = canController.begin(MCP_ANY, CAN_125KBPS, candidates[i].clock);
    if (initResult == CAN_OK)
    {
      detectedCanClock = candidates[i].clock;
      canControllerDetected = true;
      Serial.print(F("CAN init OK with "));
      Serial.println(candidates[i].label);
      return true;
    }

    Serial.print(F("CAN init failed code="));
    Serial.println(initResult);

    // Some MCP2515 + level-shifter combinations complete config/bitrate setup
    // but fail the library's return-to-previous-mode step. In that case we still
    // have SPI communication and can force mode transitions explicitly.
    const INT8U configModeResult = canController.setMode(MODE_CONFIG);
    if (configModeResult == CAN_OK)
    {
      detectedCanClock = candidates[i].clock;
      canControllerDetected = true;
      Serial.print(F("CAN fallback: config mode OK with "));
      Serial.println(candidates[i].label);
      return true;
    }

    Serial.print(F("CAN fallback config mode failed code="));
    Serial.println(configModeResult);
  }

  canControllerDetected = false;
  return false;
}

bool runCanControllerSelfTest()
{
  canLoopbackPassed = false;
  canNormalModeActive = false;

  if (!detectCanControllerClock())
  {
    Serial.println(F("CAN SELFTEST FAIL: no SPI communication with MCP2515"));
    return false;
  }

  bool loopbackModeOk = false;
  for (uint8_t retry = 0; retry < 5; retry++)
  {
    if (canController.setMode(MCP_LOOPBACK) == CAN_OK)
    {
      loopbackModeOk = true;
      break;
    }
    delay(10);
  }

  if (!loopbackModeOk)
  {
    Serial.println(F("CAN SELFTEST PARTIAL: config mode works, loopback mode unavailable"));
    return true;
  }

  INT8U txData[2] = {0xA5, 0x5A};
  const INT32U txId = 0x321;

  if (canController.sendMsgBuf(txId, 0, 2, txData) != CAN_OK)
  {
    Serial.println(F("CAN SELFTEST FAIL: loopback send failed"));
    return false;
  }

  const unsigned long startMs = millis();
  while (millis() - startMs < 120)
  {
    if (canController.checkReceive() == CAN_MSGAVAIL)
    {
      INT32U rxId = 0;
      INT8U rxLen = 0;
      INT8U rxData[8] = {0};
      if (canController.readMsgBuf(&rxId, &rxLen, rxData) != CAN_OK)
      {
        Serial.println(F("CAN SELFTEST FAIL: loopback read failed"));
        return false;
      }

      if (rxId == txId && rxLen == 2 && rxData[0] == txData[0] && rxData[1] == txData[1])
      {
        canLoopbackPassed = true;
        Serial.print(F("CAN SELFTEST PASS: MCP2515 reachable, clock="));
        Serial.println(canClockToText(detectedCanClock));
        return true;
      }

      Serial.println(F("CAN SELFTEST FAIL: loopback payload mismatch"));
      return false;
    }
  }

  Serial.println(F("CAN SELFTEST FAIL: no loopback frame received"));
  return false;
}

bool switchToCanNormalMode()
{
  if (!canControllerDetected)
  {
    Serial.println(F("CAN NORMAL MODE FAIL: controller not detected"));
    canNormalModeActive = false;
    return false;
  }

  INT8U modeResult = CAN_FAIL;
  for (uint8_t retry = 0; retry < 5; retry++)
  {
    modeResult = canController.setMode(MCP_NORMAL);
    if (modeResult == CAN_OK)
    {
      break;
    }
    delay(10);
  }

  if (modeResult != CAN_OK)
  {
    Serial.print(F("CAN NORMAL MODE FAIL code="));
    Serial.println(modeResult);
    canNormalModeActive = false;
    return false;
  }

  canNormalModeActive = true;
  Serial.println(F("CAN NORMAL MODE OK"));
  return true;
}

void pollCanFrames()
{
  if (!canNormalModeActive)
  {
    return;
  }

  while (canController.checkReceive() == CAN_MSGAVAIL)
  {
    INT32U rxId = 0;
    INT8U rxLen = 0;
    INT8U rxData[8] = {0};
    const INT8U rxResult = canController.readMsgBuf(&rxId, &rxLen, rxData);
    if (rxResult != CAN_OK)
    {
      Serial.print(F("CAN RX FAIL code="));
      Serial.println(rxResult);
      break;
    }

    canRxFrameCount++;
    const bool isExtended = (rxId & CAN_IS_EXTENDED) != 0;
    const bool isRtr = (rxId & CAN_IS_REMOTE_REQUEST) != 0;
    const uint32_t cleanId = isExtended ? (rxId & CAN_EXTENDED_ID) : (rxId & 0x7FFu);

    // For TLP-CAN/1 bring-up we expect standard 11-bit data frames.
    // Extended/RTR frames are logged as ignored to spot bus corruption quickly.
    if (isExtended || isRtr)
    {
      canRxIgnoredCount++;
      // Print first 5 ext/rtr frames to diagnose misclassification
      if (canRxIgnoredCount <= 5u)
      {
        Serial.print(F("CAN RX ext/rtr #"));
        Serial.print(canRxIgnoredCount);
        Serial.print(F(" raw_id=0x"));
        Serial.print(rxId, HEX);
        Serial.print(F(" clean_id=0x"));
        Serial.print(cleanId, HEX);
        Serial.print(F(" ext="));
        Serial.print(isExtended ? 1 : 0);
        Serial.print(F(" rtr="));
        Serial.print(isRtr ? 1 : 0);
        Serial.print(F(" len="));
        Serial.println(rxLen);
      }
      else if ((canRxIgnoredCount % 25u) == 0u)
      {
        Serial.print(F("CAN RX ignored ext/rtr frames="));
        Serial.println(canRxIgnoredCount);
      }
      continue;
    }

    const uint8_t parsedPriority = static_cast<uint8_t>((cleanId >> 9) & 0x03u);
    const uint8_t parsedMsgType = static_cast<uint8_t>((cleanId >> 5) & 0x0Fu);
    const uint8_t parsedNodeId = static_cast<uint8_t>(cleanId & 0x1Fu);

    Serial.print(F("CAN RX id=0x"));
    Serial.print(cleanId, HEX);
    Serial.print(F(" ext="));
    Serial.print(isExtended ? 1 : 0);
    Serial.print(F(" rtr="));
    Serial.print(isRtr ? 1 : 0);
    Serial.print(F(" pri="));
    Serial.print(parsedPriority);
    Serial.print(F(" type=0x"));
    Serial.print(parsedMsgType, HEX);
    Serial.print(F(" node=0x"));
    Serial.print(parsedNodeId, HEX);
    Serial.print(F(" len="));
    Serial.print(rxLen);
    Serial.print(F(" count="));
    Serial.println(canRxFrameCount);
  }
}

void printCanStatus(unsigned long nowMs)
{
  if (nowMs - lastCanStatusAtMs < CAN_STATUS_INTERVAL_MS)
  {
    return;
  }

  lastCanStatusAtMs = nowMs;
  Serial.print(F("CAN status detected="));
  Serial.print(canControllerDetected ? 1 : 0);
  Serial.print(F(" loopback="));
  Serial.print(canLoopbackPassed ? 1 : 0);
  Serial.print(F(" normal="));
  Serial.print(canNormalModeActive ? 1 : 0);
  Serial.print(F(" clock="));
  Serial.print(canClockToText(detectedCanClock));
  Serial.print(F(" rx_count="));
  Serial.print(canRxFrameCount);
  Serial.print(F(" rx_ignored="));
  Serial.print(canRxIgnoredCount);
  Serial.print(F(" rx_err="));
  Serial.print(canController.errorCountRX());
  Serial.print(F(" tx_err="));
  Serial.println(canController.errorCountTX());
}

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
  (void)RELAY_ACTIVE_HIGH;
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
    setFlap(true);
    if (printFeedback)
    {
      Serial.println(F("CMD reset (latched=0, flap=up)"));
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
  Serial.println(F("  c = run MCP2515 selftest"));
  Serial.println(F("  n = switch MCP2515 to normal mode"));
  Serial.println(F("  k = print CAN status now"));
  Serial.println(F("  x = send one test CAN frame (TX transceiver test)"));
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
  else if (single == 'c' || single == 'C')
  {
    Serial.println(F("CMD can_selftest"));
    if (runCanControllerSelfTest())
    {
      switchToCanNormalMode();
    }
  }
  else if (single == 'n' || single == 'N')
  {
    Serial.println(F("CMD can_normal_mode"));
    switchToCanNormalMode();
  }
  else if (single == 'k' || single == 'K')
  {
    lastCanStatusAtMs = 0;
    printCanStatus(millis());
  }
  else if (single == 'x' || single == 'X')
  {
    if (!canNormalModeActive)
    {
      Serial.println(F("CMD can_tx_test SKIP: not in normal mode"));
    }
    else
    {
      INT8U testData[4] = {0xDE, 0xAD, 0xBE, 0xEF};
      const INT32U testId = 0x7FF;
      const INT8U txResult = canController.sendMsgBuf(testId, 0, 4, testData);
      Serial.print(F("CMD can_tx_test id=0x7FF code="));
      Serial.print(txResult);
      Serial.println(txResult == CAN_OK ? F(" OK") : F(" FAIL"));
    }
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
                     c=='p'||c=='P'||c=='c'||c=='C'||c=='n'||c=='N'||
                     c=='k'||c=='K'||c=='x'||c=='X'||c=='?'))
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
  json.reserve(420);
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
  json += F(",\"raw\":");
  json += lastRaw;
  json += F(",\"baseline\":");
  json += lastBaseline;
  json += F(",\"centered\":");
  json += lastCentered;
  json += F(",\"envelope\":");
  json += envelope;
  json += F(",\"lastTriggerRaw\":");
  json += lastTriggerRaw;
  json += F(",\"lastTriggerEnvelope\":");
  json += lastTriggerEnvelope;
  json += F(",\"lastTriggerAt\":");
  json += lastTriggerAtMs;
  json += F(",\"canDetected\":");
  json += canControllerDetected ? 1 : 0;
  json += F(",\"canLoopback\":");
  json += canLoopbackPassed ? 1 : 0;
  json += F(",\"canNormal\":");
  json += canNormalModeActive ? 1 : 0;
  json += F(",\"canRxCount\":");
  json += canRxFrameCount;
  json += F(",\"canClockMHz\":");
  if (detectedCanClock == MCP_8MHZ) json += 8;
  else if (detectedCanClock == MCP_16MHZ) json += 16;
  else if (detectedCanClock == MCP_10MHZ) json += 10;
  else json += 0;
  json += F("}");
  return json;
}

void startWebServer()
{
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
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  pinMode(MCP2515_INT_PIN, INPUT);
  setFlap(true);

  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("TLP-Klappen NodeMCU"));
  Serial.print(F("CAN prep: cs="));
  Serial.print(MCP2515_CS_PIN);
  Serial.print(F(" int="));
  Serial.println(MCP2515_INT_PIN);

  baselineQ8 = static_cast<long>(analogRead(SENSOR_PIN)) << 8;

  if (!LittleFS.begin())
  {
    Serial.println(F("LittleFS mount failed! Run: pio run -t uploadfs"));
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

  if (runCanControllerSelfTest())
  {
    switchToCanNormalMode();
  }

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

  if (nowMs - lastSensorSampleAtMs >= SENSOR_SAMPLE_INTERVAL_MS)
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

  pollCanFrames();
  printCanStatus(nowMs);

  // Automatic hit trigger: envelope over threshold -> flap up + latch.
  if (!manualOverrideEnabled && !hitLatched && envelope >= hitThreshold)
  {
    hitLatched = true;
    lastTriggerRaw = lastRaw;
    lastTriggerEnvelope = envelope;
    lastTriggerAtMs = nowMs;
    setFlap(false);
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
