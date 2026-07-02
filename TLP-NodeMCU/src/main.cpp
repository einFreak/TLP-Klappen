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
const uint8_t MCP2515_CS_PIN = D1;
const uint8_t MCP2515_INT_PIN = D0;
const unsigned long CAN_STATUS_INTERVAL_MS = 5000;
const unsigned long SSE_STREAM_INTERVAL_MS = 4000;
const uint8_t CAN_RX_MAX_FRAMES_PER_POLL = 6;
const uint8_t CAN_RX_LOG_EVERY_N = 10;
const uint16_t STATUS_JSON_RESERVE_COMPACT = 560;
const uint16_t STATUS_JSON_RESERVE_FULL = 900;
const unsigned long NODE_HEARTBEAT_TIMEOUT_MS = 3500;
const uint8_t TLP_CAN_MSG_HEARTBEAT = 0x1;
const uint8_t TLP_CAN_MSG_SET_THRESHOLD = 0x2;
const uint8_t TLP_CAN_MSG_FLAP_COMMAND = 0x3;
const uint8_t TLP_CAN_MSG_RESULT = 0x4;
const uint8_t TLP_CAN_PRIORITY_COMMAND = 0b01;
const uint8_t TLP_CAN_PRIORITY_CYCLIC = 0b10;
const uint8_t TLP_CAN_MAX_NODES = 32;
const uint8_t MAX_PENDING_COMMANDS = 8;
const unsigned long PENDING_COMMAND_TTL_MS = 15000;

const uint8_t CAN_RESULT_OK = 0;
const uint8_t CAN_RESULT_INVALID_ARG = 1;
const uint8_t CAN_RESULT_UNSUPPORTED = 2;

const uint8_t CAN_FLAP_UP = 0;
const uint8_t CAN_FLAP_DOWN = 1;
const uint8_t CAN_FLAP_TOGGLE = 2;
const uint8_t CAN_FLAP_AUTO = 3;
const uint8_t CAN_FLAP_RESET = 4;

unsigned long lastCanStatusAtMs = 0;

unsigned long lastStatusAtMs = 0;
const unsigned long STATUS_INTERVAL_MS = 10000;
bool flapIsUp = false;
bool manualOverrideEnabled = false;

bool canControllerDetected = false;
bool canLoopbackPassed = false;
bool canNormalModeActive = false;
uint32_t canRxFrameCount = 0;
uint32_t canRxIgnoredCount = 0;
uint32_t canResultFrameCount = 0;
uint32_t canResultSequence = 0;

struct CanCommandResult
{
  bool valid;
  uint8_t sourceNodeId;
  uint8_t requestMsgType;
  uint8_t requestId;
  uint8_t status;
  uint8_t data0;
  uint8_t data1;
  uint32_t sequence;
  unsigned long receivedAtMs;
};

CanCommandResult lastCanCommandResult = {};

enum class PendingCommandState : uint8_t
{
  Pending = 0,
  Ack = 1,
  Nack = 2,
  SendFailed = 3,
  Expired = 4,
};

struct PendingCommand
{
  bool used;
  uint8_t requestId;
  uint8_t targetNodeId;
  uint8_t msgType;
  PendingCommandState state;
  uint8_t resultStatus;
  uint8_t data0;
  uint8_t data1;
  unsigned long createdAtMs;
  unsigned long updatedAtMs;
};

PendingCommand pendingCommands[MAX_PENDING_COMMANDS] = {};
uint8_t nextRequestId = 1;

struct NodeState
{
  bool seen;
  uint8_t nodeId;
  uint8_t lastPriority;
  uint8_t lastMsgType;
  uint8_t statusFlags;
  uint8_t protoMajor;
  uint8_t protoMinor;
  uint8_t fwVersion;
  uint32_t uptimeSeconds;
  uint32_t lastUptimeSeconds;
  uint32_t rebootCount;
  unsigned long lastHeartbeatAtMs;
  uint32_t heartbeatCount;
};

NodeState nodeStates[TLP_CAN_MAX_NODES] = {};

INT8U detectedCanClock = MCP_8MHZ;
MCP_CAN canController(MCP2515_CS_PIN);

AsyncWebServer webServer(80);
AsyncEventSource webEvents("/events");

WiFiEventHandler wifiApStationConnectedHandler;
WiFiEventHandler wifiApStationDisconnectedHandler;

void pollCanFrames();

const __FlashStringHelper *pendingStateToText(PendingCommandState state)
{
  if (state == PendingCommandState::Pending) return F("pending");
  if (state == PendingCommandState::Ack) return F("ack");
  if (state == PendingCommandState::Nack) return F("nack");
  if (state == PendingCommandState::SendFailed) return F("send_failed");
  return F("expired");
}

void cleanupExpiredPendingCommands(unsigned long nowMs)
{
  for (uint8_t i = 0; i < MAX_PENDING_COMMANDS; i++)
  {
    PendingCommand &entry = pendingCommands[i];
    if (!entry.used)
    {
      continue;
    }

    if (entry.state == PendingCommandState::Pending && (nowMs - entry.createdAtMs) > PENDING_COMMAND_TTL_MS)
    {
      entry.state = PendingCommandState::Expired;
      entry.updatedAtMs = nowMs;
    }

    if (entry.state != PendingCommandState::Pending && (nowMs - entry.updatedAtMs) > PENDING_COMMAND_TTL_MS)
    {
      entry.used = false;
    }
  }
}

PendingCommand *findPendingCommand(uint8_t requestId)
{
  for (uint8_t i = 0; i < MAX_PENDING_COMMANDS; i++)
  {
    if (pendingCommands[i].used && pendingCommands[i].requestId == requestId)
    {
      return &pendingCommands[i];
    }
  }
  return nullptr;
}

PendingCommand *reservePendingCommand(uint8_t targetNodeId, uint8_t msgType, unsigned long nowMs)
{
  cleanupExpiredPendingCommands(nowMs);

  PendingCommand *slot = nullptr;
  for (uint8_t i = 0; i < MAX_PENDING_COMMANDS; i++)
  {
    if (!pendingCommands[i].used)
    {
      slot = &pendingCommands[i];
      break;
    }
  }

  if (slot == nullptr)
  {
    return nullptr;
  }

  slot->used = true;
  slot->requestId = nextRequestId++;
  if (nextRequestId == 0)
  {
    nextRequestId = 1;
  }
  slot->targetNodeId = targetNodeId;
  slot->msgType = msgType;
  slot->state = PendingCommandState::Pending;
  slot->resultStatus = 0xFF;
  slot->data0 = 0;
  slot->data1 = 0;
  slot->createdAtMs = nowMs;
  slot->updatedAtMs = nowMs;
  return slot;
}

NodeState &getNodeState(uint8_t nodeId)
{
  return nodeStates[nodeId & 0x1Fu];
}

uint16_t buildCanId(uint8_t priority, uint8_t msgType, uint8_t nodeId)
{
  return (static_cast<uint16_t>(priority & 0x03u) << 9) |
         (static_cast<uint16_t>(msgType & 0x0Fu) << 5) |
         static_cast<uint16_t>(nodeId & 0x1Fu);
}

void rememberCanResult(uint8_t sourceNodeId, const INT8U *rxData, INT8U rxLen, unsigned long nowMs)
{
  if (rxLen < 3)
  {
    return;
  }

  canResultFrameCount++;
  canResultSequence++;
  lastCanCommandResult.valid = true;
  lastCanCommandResult.sourceNodeId = sourceNodeId;
  lastCanCommandResult.requestMsgType = rxData[0];
  lastCanCommandResult.requestId = rxData[1];
  lastCanCommandResult.status = rxData[2];
  lastCanCommandResult.data0 = rxLen >= 4 ? rxData[3] : 0;
  lastCanCommandResult.data1 = rxLen >= 5 ? rxData[4] : 0;
  lastCanCommandResult.sequence = canResultSequence;
  lastCanCommandResult.receivedAtMs = nowMs;

  PendingCommand *pending = findPendingCommand(lastCanCommandResult.requestId);
  if (pending != nullptr &&
      pending->targetNodeId == sourceNodeId &&
      pending->msgType == lastCanCommandResult.requestMsgType)
  {
    pending->resultStatus = lastCanCommandResult.status;
    pending->data0 = lastCanCommandResult.data0;
    pending->data1 = lastCanCommandResult.data1;
    pending->updatedAtMs = nowMs;
    pending->state = (lastCanCommandResult.status == CAN_RESULT_OK)
      ? PendingCommandState::Ack
      : PendingCommandState::Nack;
  }
}

bool sendCanCommand(uint8_t targetNodeId,
                    uint8_t requestMsgType,
                    uint8_t requestId,
                    const INT8U *payload,
                    INT8U payloadLen)
{
  if (!canNormalModeActive)
  {
    return false;
  }

  const uint16_t canId = buildCanId(TLP_CAN_PRIORITY_COMMAND, requestMsgType, targetNodeId);
  INT8U txPayload[8] = {0};
  txPayload[0] = requestId;
  const INT8U safeLen = payloadLen > 7 ? 7 : payloadLen;
  for (INT8U i = 0; i < safeLen; i++)
  {
    txPayload[i + 1] = payload[i];
  }

  const INT8U txResult = canController.sendMsgBuf(canId, 0, safeLen + 1, txPayload);
  if (txResult != CAN_OK)
  {
    Serial.print(F("CAN CMD tx_fail code="));
    Serial.println(txResult);
    return false;
  }
  return true;
}

bool isNodeOnline(const NodeState &state, unsigned long nowMs)
{
  return state.seen && ((nowMs - state.lastHeartbeatAtMs) <= NODE_HEARTBEAT_TIMEOUT_MS);
}

uint8_t countOnlineNodes(unsigned long nowMs)
{
  uint8_t online = 0;
  for (uint8_t i = 0; i < TLP_CAN_MAX_NODES; i++)
  {
    if (isNodeOnline(nodeStates[i], nowMs))
    {
      online++;
    }
  }
  return online;
}

void updateNodeHeartbeat(uint8_t parsedPriority, uint8_t nodeId, const INT8U *rxData, INT8U rxLen, unsigned long nowMs)
{
  if (rxLen < 8)
  {
    return;
  }

  NodeState &state = getNodeState(nodeId);
  state.seen = true;
  state.nodeId = nodeId;
  state.lastPriority = parsedPriority;
  state.lastMsgType = TLP_CAN_MSG_HEARTBEAT;
  state.protoMajor = rxData[0];
  state.protoMinor = rxData[1];
  state.fwVersion = rxData[2];
  state.statusFlags = rxData[3];
  // ATmega heartbeat encodes uptime as little-endian bytes 4..7.
  const uint32_t parsedUptimeSeconds = static_cast<uint32_t>(rxData[4]) |
                                       (static_cast<uint32_t>(rxData[5]) << 8) |
                                       (static_cast<uint32_t>(rxData[6]) << 16) |
                                       (static_cast<uint32_t>(rxData[7]) << 24);
  if (state.heartbeatCount > 0 && parsedUptimeSeconds < state.uptimeSeconds)
  {
    state.rebootCount++;
  }

  state.lastUptimeSeconds = state.uptimeSeconds;
  state.uptimeSeconds = parsedUptimeSeconds;
  state.lastHeartbeatAtMs = nowMs;
  state.heartbeatCount++;

  const bool remoteRelayPulseActive = (state.statusFlags & (1u << 2)) != 0;
  const bool remoteAutoMode = (state.statusFlags & (1u << 1)) != 0;
  flapIsUp = !remoteRelayPulseActive;
  manualOverrideEnabled = !remoteAutoMode;
}

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

  uint8_t processedFrames = 0;
  while ((processedFrames < CAN_RX_MAX_FRAMES_PER_POLL) && (canController.checkReceive() == CAN_MSGAVAIL))
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

    processedFrames++;
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

    const unsigned long nowMs = millis();
    const uint8_t parsedPriority = static_cast<uint8_t>((cleanId >> 9) & 0x03u);
    const uint8_t parsedMsgType = static_cast<uint8_t>((cleanId >> 5) & 0x0Fu);
    const uint8_t parsedNodeId = static_cast<uint8_t>(cleanId & 0x1Fu);

    if (parsedMsgType == TLP_CAN_MSG_HEARTBEAT)
    {
      updateNodeHeartbeat(parsedPriority, parsedNodeId, rxData, rxLen, nowMs);
    }
    else if (parsedMsgType == TLP_CAN_MSG_RESULT)
    {
      rememberCanResult(parsedNodeId, rxData, rxLen, nowMs);
    }

    if ((canRxFrameCount <= 5u) || ((canRxFrameCount % CAN_RX_LOG_EVERY_N) == 0u))
    {
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

    // Keep WiFi and AsyncWebServer tasks responsive while draining CAN RX.
    yield();
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
  Serial.print(canController.errorCountTX());
  Serial.print(F(" result_count="));
  Serial.print(canResultFrameCount);
  Serial.print(F(" online_nodes="));
  Serial.print(countOnlineNodes(nowMs));
  Serial.println();
}

void setFlap(bool moveUp)
{
  flapIsUp = moveUp;
  (void)RELAY_ACTIVE_HIGH;
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

void writeStatusJson(Print &out, bool includeExtendedNodes)
{
  const unsigned long nowMs = millis();
  out.print(F("{\"ssid\":\""));
  out.print(AP_SSID);
  out.print(F("\",\"ip\":\""));
  out.print(WiFi.softAPIP().toString());
  out.print(F("\",\"channel\":"));
  out.print(WiFi.channel());
  out.print(F(",\"clients\":"));
  out.print(WiFi.softAPgetStationNum());
  out.print(F(",\"mac\":\""));
  out.print(WiFi.softAPmacAddress());
  out.print(F("\""));
  out.print(F(",\"flap\":"));
  out.print(flapIsUp ? 1 : 0);
  out.print(F(",\"mode\":\""));
  out.print(manualOverrideEnabled ? F("manual") : F("auto"));
  out.print(F("\""));
  out.print(F(",\"canDetected\":"));
  out.print(canControllerDetected ? 1 : 0);
  out.print(F(",\"canLoopback\":"));
  out.print(canLoopbackPassed ? 1 : 0);
  out.print(F(",\"canNormal\":"));
  out.print(canNormalModeActive ? 1 : 0);
  out.print(F(",\"canRxCount\":"));
  out.print(canRxFrameCount);
  out.print(F(",\"canRxIgnored\":"));
  out.print(canRxIgnoredCount);
  out.print(F(",\"canResultCount\":"));
  out.print(canResultFrameCount);
  out.print(F(",\"onlineNodes\":"));
  out.print(countOnlineNodes(nowMs));
  out.print(F(",\"canClockMHz\":"));
  if (detectedCanClock == MCP_8MHZ) out.print(8);
  else if (detectedCanClock == MCP_16MHZ) out.print(16);
  else if (detectedCanClock == MCP_10MHZ) out.print(10);
  else out.print(0);

  if (includeExtendedNodes)
  {
    out.print(F(",\"nodes\":["));
    bool firstNode = true;
    for (uint8_t i = 0; i < TLP_CAN_MAX_NODES; i++)
    {
      const NodeState &state = nodeStates[i];
      if (!state.seen)
      {
        continue;
      }

      if (!firstNode)
      {
        out.print(',');
      }
      firstNode = false;

      out.print(F("{\"id\":"));
      out.print(state.nodeId);
      out.print(F(",\"online\":"));
      out.print(isNodeOnline(state, nowMs) ? 1 : 0);
      out.print(F(",\"hb\":"));
      out.print(state.heartbeatCount);
      out.print(F(",\"reboots\":"));
      out.print(state.rebootCount);
      out.print(F(",\"flags\":"));
      out.print(state.statusFlags);
      out.print(F(",\"uptimeSeconds\":"));
      out.print(state.uptimeSeconds);
      out.print(F("}"));
    }
    out.print(F("]"));
  }
  out.print(F("}"));
}

void sendStatusJsonResponse(AsyncWebServerRequest *request, bool includeExtendedNodes)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  writeStatusJson(*response, includeExtendedNodes);
  request->send(response);
}

void sendCommandAcceptedResponse(AsyncWebServerRequest *request, const PendingCommand &entry)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->print(F("{\"ok\":1,\"pending\":1,\"requestId\":"));
  response->print(entry.requestId);
  response->print(F(",\"node\":"));
  response->print(entry.targetNodeId);
  response->print(F("}"));
  response->setCode(202);
  request->send(response);
}

void sendCommandResultResponse(AsyncWebServerRequest *request, const PendingCommand &entry)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->print(F("{\"requestId\":"));
  response->print(entry.requestId);
  response->print(F(",\"node\":"));
  response->print(entry.targetNodeId);
  response->print(F(",\"msgType\":"));
  response->print(entry.msgType);
  response->print(F(",\"state\":\""));
  response->print(pendingStateToText(entry.state));
  response->print(F("\",\"done\":"));
  response->print(entry.state == PendingCommandState::Pending ? 0 : 1);
  response->print(F(",\"status\":"));
  response->print(entry.resultStatus);
  response->print(F(",\"data0\":"));
  response->print(entry.data0);
  response->print(F(",\"data1\":"));
  response->print(entry.data1);
  response->print(F("}"));
  request->send(response);
}

bool parseNodeIdParam(AsyncWebServerRequest *request, uint8_t &nodeId)
{
  String nodeValue;
  if (request->hasParam("node", true))
  {
    nodeValue = request->getParam("node", true)->value();
  }
  else if (request->hasParam("node"))
  {
    nodeValue = request->getParam("node")->value();
  }

  if (nodeValue.length() == 0)
  {
    return false;
  }

  const long parsedNode = strtol(nodeValue.c_str(), nullptr, 10);
  if (parsedNode < 0 || parsedNode > 31)
  {
    return false;
  }

  nodeId = static_cast<uint8_t>(parsedNode);
  return true;
}

String buildStatusJson(bool includeExtendedNodes)
{
  String json;
  json.reserve(includeExtendedNodes ? STATUS_JSON_RESERVE_FULL : STATUS_JSON_RESERVE_COMPACT);
  const unsigned long nowMs = millis();
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
  json += F(",\"mode\":\"");
  json += manualOverrideEnabled ? F("manual") : F("auto");
  json += F("\"");
  json += F(",\"canDetected\":");
  json += canControllerDetected ? 1 : 0;
  json += F(",\"canLoopback\":");
  json += canLoopbackPassed ? 1 : 0;
  json += F(",\"canNormal\":");
  json += canNormalModeActive ? 1 : 0;
  json += F(",\"canRxCount\":");
  json += canRxFrameCount;
  json += F(",\"canRxIgnored\":");
  json += canRxIgnoredCount;
  json += F(",\"canResultCount\":");
  json += canResultFrameCount;
  json += F(",\"onlineNodes\":");
  json += countOnlineNodes(nowMs);
  json += F(",\"canClockMHz\":");
  if (detectedCanClock == MCP_8MHZ) json += 8;
  else if (detectedCanClock == MCP_16MHZ) json += 16;
  else if (detectedCanClock == MCP_10MHZ) json += 10;
  else json += 0;

  if (includeExtendedNodes)
  {
    json += F(",\"nodes\":[");
    bool firstNode = true;
    for (uint8_t i = 0; i < TLP_CAN_MAX_NODES; i++)
    {
      const NodeState &state = nodeStates[i];
      if (!state.seen)
      {
        continue;
      }

      if (!firstNode)
      {
        json += ',';
      }
      firstNode = false;

      json += F("{\"id\":");
      json += state.nodeId;
      json += F(",\"online\":");
      json += isNodeOnline(state, nowMs) ? 1 : 0;
      json += F(",\"hb\":");
      json += state.heartbeatCount;
      json += F(",\"reboots\":");
      json += state.rebootCount;
      json += F(",\"flags\":");
      json += state.statusFlags;
      json += F(",\"uptimeSeconds\":");
      json += state.uptimeSeconds;
      json += F("}");
    }
    json += F("]");
  }
  json += F("}");
  return json;
}

void startWebServer()
{
  // API routes MUST be registered before serveStatic, otherwise the
  // catch-all static handler intercepts them and returns 404.
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendStatusJsonResponse(request, true);
  });

  webServer.on("/api/flap", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action;
    if      (request->hasParam("action", true)) action = request->getParam("action", true)->value();
    else if (request->hasParam("action"))       action = request->getParam("action")->value();
    action.toLowerCase();

    uint8_t flapCommand = CAN_FLAP_TOGGLE;
    if      (action == "up")     flapCommand = CAN_FLAP_UP;
    else if (action == "down")   flapCommand = CAN_FLAP_DOWN;
    else if (action == "toggle") flapCommand = CAN_FLAP_TOGGLE;
    else if (action == "auto")   flapCommand = CAN_FLAP_AUTO;
    else if (action == "reset")  flapCommand = CAN_FLAP_RESET;
    else { request->send(400, "application/json", "{\"error\":\"invalid action\"}"); return; }

    uint8_t targetNodeId = 0;
    if (!parseNodeIdParam(request, targetNodeId))
    {
      request->send(400, "application/json", "{\"error\":\"missing or invalid node\"}");
      return;
    }

    const unsigned long nowMs = millis();
    PendingCommand *pending = reservePendingCommand(targetNodeId, TLP_CAN_MSG_FLAP_COMMAND, nowMs);
    if (pending == nullptr)
    {
      request->send(503, "application/json", "{\"error\":\"pending queue full\"}");
      return;
    }

    INT8U payload[1] = {flapCommand};
    if (!sendCanCommand(targetNodeId, TLP_CAN_MSG_FLAP_COMMAND, pending->requestId, payload, 1))
    {
      pending->state = PendingCommandState::SendFailed;
      pending->updatedAtMs = nowMs;
      request->send(503, "application/json", "{\"error\":\"can send failed\"}");
      return;
    }

    sendCommandAcceptedResponse(request, *pending);
  });

  webServer.on("/api/threshold", HTTP_POST, [](AsyncWebServerRequest *request) {
    String val;
    if      (request->hasParam("value", true)) val = request->getParam("value", true)->value();
    else if (request->hasParam("value"))       val = request->getParam("value")->value();
    if (val.length() == 0) { request->send(400, "application/json", "{\"error\":\"missing value\"}"); return; }
    const long parsed = strtol(val.c_str(), nullptr, 10);
    const int threshold = static_cast<int>(parsed);
    if (threshold < 0 || threshold > 1023)
    { request->send(400, "application/json", "{\"error\":\"value out of range\"}"); return; }

    uint8_t targetNodeId = 0;
    if (!parseNodeIdParam(request, targetNodeId))
    {
      request->send(400, "application/json", "{\"error\":\"missing or invalid node\"}");
      return;
    }

    const unsigned long nowMs = millis();
    PendingCommand *pending = reservePendingCommand(targetNodeId, TLP_CAN_MSG_SET_THRESHOLD, nowMs);
    if (pending == nullptr)
    {
      request->send(503, "application/json", "{\"error\":\"pending queue full\"}");
      return;
    }

    INT8U payload[2] = {
      static_cast<INT8U>(threshold & 0xFF),
      static_cast<INT8U>((threshold >> 8) & 0xFF)
    };
    if (!sendCanCommand(targetNodeId, TLP_CAN_MSG_SET_THRESHOLD, pending->requestId, payload, 2))
    {
      pending->state = PendingCommandState::SendFailed;
      pending->updatedAtMs = nowMs;
      request->send(503, "application/json", "{\"error\":\"can send failed\"}");
      return;
    }

    sendCommandAcceptedResponse(request, *pending);
  });

  webServer.on("/api/command-result", HTTP_GET, [](AsyncWebServerRequest *request) {
    String idValue;
    if (request->hasParam("id"))
    {
      idValue = request->getParam("id")->value();
    }
    else
    {
      request->send(400, "application/json", "{\"error\":\"missing id\"}");
      return;
    }

    const long parsedId = strtol(idValue.c_str(), nullptr, 10);
    if (parsedId < 1 || parsedId > 255)
    {
      request->send(400, "application/json", "{\"error\":\"invalid id\"}");
      return;
    }

    cleanupExpiredPendingCommands(millis());
    PendingCommand *entry = findPendingCommand(static_cast<uint8_t>(parsedId));
    if (entry == nullptr)
    {
      request->send(404, "application/json", "{\"error\":\"unknown request\"}");
      return;
    }

    sendCommandResultResponse(request, *entry);
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
  static uint32_t sseDropped = 0;
  const unsigned long nowMs = millis();

  if (nowMs - lastStreamAtMs >= SSE_STREAM_INTERVAL_MS)
  {
    lastStreamAtMs = nowMs;
    if (webEvents.count() == 0)
    {
      return;
    }

    const String status = buildStatusJson(true);
    const AsyncEventSource::SendStatus sendStatus = webEvents.send(status.c_str(), "status", nowMs);
    if (sendStatus != AsyncEventSource::SendStatus::ENQUEUED)
    {
      sseDropped++;
      if ((sseDropped % 10u) == 0u)
      {
        Serial.print(F("SSE backpressure drops="));
        Serial.print(sseDropped);
        Serial.print(F(" clients="));
        Serial.print(webEvents.count());
        Serial.print(F(" queued_avg="));
        Serial.println(webEvents.avgPacketsWaiting());
      }
    }
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
}

void loop()
{
  const unsigned long nowMs = millis();

  cleanupExpiredPendingCommands(nowMs);

  if (nowMs - lastStatusAtMs >= STATUS_INTERVAL_MS)
  {
    lastStatusAtMs = nowMs;
    printApStatus();
  }

  pollCanFrames();
  printCanStatus(nowMs);

  streamApStatus();

  // Keep WiFi background tasks responsive even with active sensor processing.
  yield();
}
