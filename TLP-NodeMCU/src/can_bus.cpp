#include "can_bus.h"

#include <Arduino.h>
#include <SPI.h>

#include "config.h"
#include "node_state.h"
#include "system_state.h"

bool canControllerDetected = false;
bool canLoopbackPassed = false;
bool canNormalModeActive = false;
uint32_t canRxFrameCount = 0;
uint32_t canRxIgnoredCount = 0;
uint32_t canResultFrameCount = 0;
uint32_t canResultSequence = 0;
unsigned long lastCanStatusAtMs = 0;

CanCommandResult lastCanCommandResult = {};
PendingCommand pendingCommands[Config::MAX_PENDING_COMMANDS] = {};
uint8_t nextRequestId = 1;

INT8U detectedCanClock = MCP_8MHZ;
MCP_CAN canController(Config::MCP2515_CS_PIN);

static uint16_t buildCanId(uint8_t priority, uint8_t msgType, uint8_t nodeId) {
  return (static_cast<uint16_t>(priority & 0x03u) << 9) |
         (static_cast<uint16_t>(msgType & 0x0Fu) << 5) |
         static_cast<uint16_t>(nodeId & 0x1Fu);
}

const __FlashStringHelper *pendingStateToText(PendingCommandState state) {
  if (state == PendingCommandState::Pending) return F("pending");
  if (state == PendingCommandState::Ack) return F("ack");
  if (state == PendingCommandState::Nack) return F("nack");
  if (state == PendingCommandState::SendFailed) return F("send_failed");
  return F("expired");
}

void cleanupExpiredPendingCommands(unsigned long nowMs) {
  for (uint8_t i = 0; i < Config::MAX_PENDING_COMMANDS; i++) {
    PendingCommand &entry = pendingCommands[i];
    if (!entry.used) {
      continue;
    }

    if (entry.state == PendingCommandState::Pending &&
        (nowMs - entry.createdAtMs) > Config::PENDING_COMMAND_TTL_MS) {
      entry.state = PendingCommandState::Expired;
      entry.updatedAtMs = nowMs;
    }

    if (entry.state != PendingCommandState::Pending &&
        (nowMs - entry.updatedAtMs) > Config::PENDING_COMMAND_TTL_MS) {
      entry.used = false;
    }
  }
}

PendingCommand *findPendingCommand(uint8_t requestId) {
  for (uint8_t i = 0; i < Config::MAX_PENDING_COMMANDS; i++) {
    if (pendingCommands[i].used && pendingCommands[i].requestId == requestId) {
      return &pendingCommands[i];
    }
  }
  return nullptr;
}

PendingCommand *reservePendingCommand(uint8_t targetNodeId, uint8_t msgType, unsigned long nowMs) {
  cleanupExpiredPendingCommands(nowMs);

  PendingCommand *slot = nullptr;
  for (uint8_t i = 0; i < Config::MAX_PENDING_COMMANDS; i++) {
    if (!pendingCommands[i].used) {
      slot = &pendingCommands[i];
      break;
    }
  }

  if (slot == nullptr) {
    return nullptr;
  }

  slot->used = true;
  slot->requestId = nextRequestId++;
  if (nextRequestId == 0) {
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

static void rememberCanResult(uint8_t sourceNodeId, const INT8U *rxData, INT8U rxLen, unsigned long nowMs) {
  if (rxLen < 3) {
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
      pending->msgType == lastCanCommandResult.requestMsgType) {
    pending->resultStatus = lastCanCommandResult.status;
    pending->data0 = lastCanCommandResult.data0;
    pending->data1 = lastCanCommandResult.data1;
    pending->updatedAtMs = nowMs;
    pending->state = (lastCanCommandResult.status == Config::CAN_RESULT_OK)
      ? PendingCommandState::Ack
      : PendingCommandState::Nack;

    if (pending->state == PendingCommandState::Ack &&
        pending->msgType == Config::TLP_CAN_MSG_FLAP_COMMAND) {
      NodeState &state = getNodeState(sourceNodeId);
      if (state.seen) {
        const bool reportedFlapUp = (pending->data1 != 0);
        if (reportedFlapUp) {
          state.statusFlags |= (1u << 0);
          state.statusFlags &= static_cast<uint8_t>(~(1u << 2));
        } else {
          state.statusFlags &= static_cast<uint8_t>(~(1u << 0));
          state.statusFlags |= (1u << 2);
        }

        if (pending->data0 == Config::CAN_FLAP_AUTO || pending->data0 == Config::CAN_FLAP_RESET) {
          state.statusFlags |= (1u << 1);
        } else {
          state.statusFlags &= static_cast<uint8_t>(~(1u << 1));
        }
      }
    }
  }
}

bool sendCanCommand(uint8_t targetNodeId,
                    uint8_t requestMsgType,
                    uint8_t requestId,
                    const INT8U *payload,
                    INT8U payloadLen) {
  if (!canNormalModeActive) {
    return false;
  }

  const uint16_t canId = buildCanId(Config::TLP_CAN_PRIORITY_COMMAND, requestMsgType, targetNodeId);
  INT8U txPayload[8] = {0};
  txPayload[0] = requestId;
  const INT8U safeLen = payloadLen > 7 ? 7 : payloadLen;
  for (INT8U i = 0; i < safeLen; i++) {
    txPayload[i + 1] = payload[i];
  }

  const INT8U txResult = canController.sendMsgBuf(canId, 0, safeLen + 1, txPayload);
  if (txResult != CAN_OK) {
    Serial.print(F("CAN CMD tx_fail code="));
    Serial.println(txResult);
    return false;
  }
  return true;
}

static void updateNodeHeartbeat(uint8_t parsedPriority, uint8_t nodeId, const INT8U *rxData, INT8U rxLen, unsigned long nowMs) {
  if (rxLen < 8) {
    return;
  }

  NodeState &state = getNodeState(nodeId);
  state.seen = true;
  state.nodeId = nodeId;
  state.lastPriority = parsedPriority;
  state.lastMsgType = Config::TLP_CAN_MSG_HEARTBEAT;
  state.protoMajor = rxData[0];
  state.protoMinor = rxData[1];
  state.fwVersion = rxData[2];
  state.statusFlags = rxData[3];
  const uint32_t parsedUptimeSeconds = static_cast<uint32_t>(rxData[4]) |
                                       (static_cast<uint32_t>(rxData[5]) << 8) |
                                       (static_cast<uint32_t>(rxData[6]) << 16) |
                                       (static_cast<uint32_t>(rxData[7]) << 24);
  if (state.heartbeatCount > 0 && parsedUptimeSeconds < state.uptimeSeconds) {
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

static void updateNodeSensorSummary(uint8_t parsedPriority, uint8_t nodeId, const INT8U *rxData, INT8U rxLen, unsigned long nowMs) {
  if (rxLen < 8) {
    return;
  }

  NodeState &state = getNodeState(nodeId);
  state.seen = true;
  state.nodeId = nodeId;
  state.lastPriority = parsedPriority;
  state.lastMsgType = Config::TLP_CAN_MSG_SENSOR_SUMMARY;

  state.currentRaw = static_cast<uint16_t>(rxData[0]) |
                     (static_cast<uint16_t>(rxData[1]) << 8);
  state.peak = static_cast<uint16_t>(rxData[2]) |
               (static_cast<uint16_t>(rxData[3]) << 8);
  state.noiseFloor = static_cast<uint16_t>(rxData[4]) |
                     (static_cast<uint16_t>(rxData[5]) << 8);
  state.sensorSummaryCounter = rxData[6];
  state.lastSensorSummaryAtMs = nowMs;
}

static void updateNodeFlapTiming(uint8_t parsedPriority, uint8_t nodeId, const INT8U *rxData, INT8U rxLen, unsigned long nowMs) {
  if (rxLen < 5) {
    return;
  }

  NodeState &state = getNodeState(nodeId);
  state.seen = true;
  state.nodeId = nodeId;
  state.lastPriority = parsedPriority;
  state.lastMsgType = Config::TLP_CAN_MSG_FLAP_TIMING;
  state.lastUpDurationMs = static_cast<uint32_t>(rxData[0]) |
                           (static_cast<uint32_t>(rxData[1]) << 8) |
                           (static_cast<uint32_t>(rxData[2]) << 16) |
                           (static_cast<uint32_t>(rxData[3]) << 24);
  state.lastUpEventCounter = rxData[4];
  state.lastFlapTimingAtMs = nowMs;
}

const __FlashStringHelper *canClockToText(INT8U clockSel) {
  if (clockSel == MCP_8MHZ) return F("8MHz");
  if (clockSel == MCP_16MHZ) return F("16MHz");
  if (clockSel == MCP_10MHZ) return F("10MHz");
  return F("unknown");
}

static bool detectCanControllerClock() {
  struct ClockCandidate {
    INT8U clock;
    const __FlashStringHelper *label;
  };

  const ClockCandidate candidates[] = {
    {MCP_8MHZ, F("8MHz")},
    {MCP_16MHZ, F("16MHz")},
    {MCP_10MHZ, F("10MHz")},
  };

  for (uint8_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    Serial.print(F("CAN init probe "));
    Serial.print(candidates[i].label);
    Serial.println(F(" ..."));

    const INT8U initResult = canController.begin(MCP_ANY, CAN_125KBPS, candidates[i].clock);
    if (initResult == CAN_OK) {
      detectedCanClock = candidates[i].clock;
      canControllerDetected = true;
      Serial.print(F("CAN init OK with "));
      Serial.println(candidates[i].label);
      return true;
    }

    Serial.print(F("CAN init failed code="));
    Serial.println(initResult);

    const INT8U configModeResult = canController.setMode(MODE_CONFIG);
    if (configModeResult == CAN_OK) {
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

bool runCanControllerSelfTest() {
  canLoopbackPassed = false;
  canNormalModeActive = false;

  if (!detectCanControllerClock()) {
    Serial.println(F("CAN SELFTEST FAIL: no SPI communication with MCP2515"));
    return false;
  }

  bool loopbackModeOk = false;
  for (uint8_t retry = 0; retry < 5; retry++) {
    if (canController.setMode(MCP_LOOPBACK) == CAN_OK) {
      loopbackModeOk = true;
      break;
    }
    delay(10);
  }

  if (!loopbackModeOk) {
    Serial.println(F("CAN SELFTEST PARTIAL: config mode works, loopback mode unavailable"));
    return true;
  }

  INT8U txData[2] = {0xA5, 0x5A};
  const INT32U txId = 0x321;

  if (canController.sendMsgBuf(txId, 0, 2, txData) != CAN_OK) {
    Serial.println(F("CAN SELFTEST FAIL: loopback send failed"));
    return false;
  }

  const unsigned long startMs = millis();
  while (millis() - startMs < 120) {
    if (canController.checkReceive() == CAN_MSGAVAIL) {
      INT32U rxId = 0;
      INT8U rxLen = 0;
      INT8U rxData[8] = {0};
      if (canController.readMsgBuf(&rxId, &rxLen, rxData) != CAN_OK) {
        Serial.println(F("CAN SELFTEST FAIL: loopback read failed"));
        return false;
      }

      if (rxId == txId && rxLen == 2 && rxData[0] == txData[0] && rxData[1] == txData[1]) {
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

bool switchToCanNormalMode() {
  if (!canControllerDetected) {
    Serial.println(F("CAN NORMAL MODE FAIL: controller not detected"));
    canNormalModeActive = false;
    return false;
  }

  INT8U modeResult = CAN_FAIL;
  for (uint8_t retry = 0; retry < 5; retry++) {
    modeResult = canController.setMode(MCP_NORMAL);
    if (modeResult == CAN_OK) {
      break;
    }
    delay(10);
  }

  if (modeResult != CAN_OK) {
    Serial.print(F("CAN NORMAL MODE FAIL code="));
    Serial.println(modeResult);
    canNormalModeActive = false;
    return false;
  }

  canNormalModeActive = true;
  Serial.println(F("CAN NORMAL MODE OK"));
  return true;
}

void pollCanFrames() {
  if (!canNormalModeActive) {
    return;
  }

  uint8_t processedFrames = 0;
  while ((processedFrames < Config::CAN_RX_MAX_FRAMES_PER_POLL) && (canController.checkReceive() == CAN_MSGAVAIL)) {
    INT32U rxId = 0;
    INT8U rxLen = 0;
    INT8U rxData[8] = {0};
    const INT8U rxResult = canController.readMsgBuf(&rxId, &rxLen, rxData);
    if (rxResult != CAN_OK) {
      Serial.print(F("CAN RX FAIL code="));
      Serial.println(rxResult);
      break;
    }

    processedFrames++;
    canRxFrameCount++;
    const bool isExtended = (rxId & CAN_IS_EXTENDED) != 0;
    const bool isRtr = (rxId & CAN_IS_REMOTE_REQUEST) != 0;
    const uint32_t cleanId = isExtended ? (rxId & CAN_EXTENDED_ID) : (rxId & 0x7FFu);

    if (isExtended || isRtr) {
      canRxIgnoredCount++;
      if (canRxIgnoredCount <= 5u) {
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
      } else if ((canRxIgnoredCount % 25u) == 0u) {
        Serial.print(F("CAN RX ignored ext/rtr frames="));
        Serial.println(canRxIgnoredCount);
      }
      continue;
    }

    const unsigned long nowMs = millis();
    const uint8_t parsedPriority = static_cast<uint8_t>((cleanId >> 9) & 0x03u);
    const uint8_t parsedMsgType = static_cast<uint8_t>((cleanId >> 5) & 0x0Fu);
    const uint8_t parsedNodeId = static_cast<uint8_t>(cleanId & 0x1Fu);

    if (parsedMsgType == Config::TLP_CAN_MSG_HEARTBEAT) {
      updateNodeHeartbeat(parsedPriority, parsedNodeId, rxData, rxLen, nowMs);
    } else if (parsedMsgType == Config::TLP_CAN_MSG_SENSOR_SUMMARY) {
      updateNodeSensorSummary(parsedPriority, parsedNodeId, rxData, rxLen, nowMs);
    } else if (parsedMsgType == Config::TLP_CAN_MSG_FLAP_TIMING) {
      updateNodeFlapTiming(parsedPriority, parsedNodeId, rxData, rxLen, nowMs);
    } else if (parsedMsgType == Config::TLP_CAN_MSG_RESULT) {
      rememberCanResult(parsedNodeId, rxData, rxLen, nowMs);
    }

    if ((canRxFrameCount <= 5u) || ((canRxFrameCount % Config::CAN_RX_LOG_EVERY_N) == 0u)) {
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

    yield();
  }
}

void printCanStatus(unsigned long nowMs) {
  if (nowMs - lastCanStatusAtMs < Config::CAN_STATUS_INTERVAL_MS) {
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
