#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>
#include <limits.h>

const uint8_t SENSOR_PIN = A0;
const uint8_t RELAY_PIN = 7;
const bool RELAY_ACTIVE_HIGH = true;

const uint8_t NODE_ID = 0x11;
const uint8_t MCP2515_CS_PIN = 10;
const uint8_t MCP2515_INT_PIN = 2;
const uint32_t CAN_BITRATE = 125000UL;
const uint8_t PROTO_MAJOR = 1;
const uint8_t PROTO_MINOR = 0;
const uint8_t FW_VERSION = 1;

const uint8_t BASELINE_FILTER_SHIFT = 4;
const uint8_t ENVELOPE_DECAY_SHIFT = 2;
const uint8_t NOISE_FLOOR_FILTER_SHIFT = 5;

const unsigned long DEBUG_INTERVAL_MS = 100;
const unsigned long REFRACTORY_MS = 220;
const unsigned long HEARTBEAT_INTERVAL_MS = 1000;
const unsigned long HEARTBEAT_INFO_LOG_EVERY = 10;
const unsigned long SHORT_BURNIN_DURATION_MS = 10UL * 60UL * 1000UL;
const unsigned long SHORT_BURNIN_PROGRESS_MS = 60UL * 1000UL;

const uint8_t CAN_PRIORITY_COMMAND = 0b01;
const uint8_t CAN_PRIORITY_CYCLIC = 0b10;
const uint8_t CAN_MSG_HEARTBEAT = 0x1;
const uint8_t CAN_MSG_SET_THRESHOLD = 0x2;
const uint8_t CAN_MSG_FLAP_COMMAND = 0x3;
const uint8_t CAN_MSG_RESULT = 0x4;
const uint8_t CAN_MSG_SENSOR_SUMMARY = 0x5;

const uint8_t CAN_RESULT_OK = 0;
const uint8_t CAN_RESULT_INVALID_ARG = 1;
const uint8_t CAN_RESULT_UNSUPPORTED = 2;

const uint8_t CAN_FLAP_UP = 0;
const uint8_t CAN_FLAP_DOWN = 1;
const uint8_t CAN_FLAP_TOGGLE = 2;
const uint8_t CAN_FLAP_AUTO = 3;
const uint8_t CAN_FLAP_RESET = 4;

int hitThreshold = 60;
long baselineQ8 = 0;
int envelope = 0;
int lastRaw = 0;
int lastBaseline = 0;
int lastCentered = 0;
int lastNoiseFloor = 0;
uint8_t eventCounter = 0;

int peakSinceLastReport = 0;
uint8_t sensorSummaryCounter = 0;

bool flapIsUp = true;
bool autoModeEnabled = true;
bool debugStreamingEnabled = false;
bool canControllerDetected = false;
bool canLoopbackPassed = false;
bool canNormalModeActive = false;

MCP_CAN canController(MCP2515_CS_PIN);

unsigned long lastHitAtMs = 0;
unsigned long lastDebugAtMs = 0;
unsigned long lastHeartbeatAtMs = 0;
unsigned long lastSensorSummaryAtMs = 0;
unsigned long bootMs = 0;
unsigned long burnInStartedAtMs = 0;
unsigned long lastBurnInLogAtMs = 0;

long noiseFloorQ8 = 0;

uint32_t heartbeatSentCount = 0;
uint32_t heartbeatErrorCount = 0;

INT8U detectedCanClock = MCP_8MHZ;

void setFlapState(bool isUp);

uint16_t buildCanId(uint8_t priority, uint8_t msgType, uint8_t nodeId) {
  return (static_cast<uint16_t>(priority & 0x03) << 9) |
         (static_cast<uint16_t>(msgType & 0x0F) << 5) |
         static_cast<uint16_t>(nodeId & 0x1F);
}

uint8_t buildStatusFlags() {
  uint8_t flags = 0;

  if (flapIsUp) {
    flags |= (1u << 0);
  }
  if (autoModeEnabled) {
    flags |= (1u << 1);
  }
  if (!flapIsUp) {
    flags |= (1u << 2);
  }

  return flags;
}

bool switchToCanNormalMode() {
  const INT8U setModeResult = canController.setMode(MCP_NORMAL);
  if (setModeResult != CAN_OK) {
    canNormalModeActive = false;
    Serial.print(F("CAN NORMAL MODE FAIL code="));
    Serial.println(setModeResult);
    return false;
  }

  canNormalModeActive = true;
  Serial.println(F("CAN NORMAL MODE OK"));
  return true;
}

void sendHeartbeat(unsigned long nowMs) {
  if (!canNormalModeActive) {
    return;
  }

  INT8U payload[8] = {0};
  payload[0] = PROTO_MAJOR;
  payload[1] = PROTO_MINOR;
  payload[2] = FW_VERSION;
  payload[3] = buildStatusFlags();

  const uint32_t uptimeS = (nowMs - bootMs) / 1000UL;
  payload[4] = static_cast<uint8_t>(uptimeS & 0xFFu);
  payload[5] = static_cast<uint8_t>((uptimeS >> 8) & 0xFFu);
  payload[6] = static_cast<uint8_t>((uptimeS >> 16) & 0xFFu);
  payload[7] = static_cast<uint8_t>((uptimeS >> 24) & 0xFFu);

  const INT32U canId = buildCanId(CAN_PRIORITY_CYCLIC, CAN_MSG_HEARTBEAT, NODE_ID);
  const INT8U txResult = canController.sendMsgBuf(canId, 0, 8, payload);
  if (txResult == CAN_OK) {
    heartbeatSentCount++;
    if ((heartbeatSentCount % HEARTBEAT_INFO_LOG_EVERY) == 0) {
      Serial.print(F("CAN HEARTBEAT info sent="));
      Serial.print(heartbeatSentCount);
      Serial.print(F(" errors="));
      Serial.println(heartbeatErrorCount);
    }
    return;
  }

  heartbeatErrorCount++;
  Serial.print(F("CAN HEARTBEAT tx_fail code="));
  Serial.println(txResult);
}

void sendSensorSummary(unsigned long nowMs) {
  if (!canNormalModeActive) {
    return;
  }

  INT8U payload[8] = {0};
  
  // Byte 0..1: current_raw (u16, little endian)
  payload[0] = static_cast<uint8_t>(lastRaw & 0xFFu);
  payload[1] = static_cast<uint8_t>((lastRaw >> 8) & 0xFFu);
  
  // Byte 2..3: peak since last report (u16, little endian)
  payload[2] = static_cast<uint8_t>(peakSinceLastReport & 0xFFu);
  payload[3] = static_cast<uint8_t>((peakSinceLastReport >> 8) & 0xFFu);
  
  // Byte 4..5: noise_floor from centered magnitude low-pass (u16, little endian)
  payload[4] = static_cast<uint8_t>(lastNoiseFloor & 0xFFu);
  payload[5] = static_cast<uint8_t>((lastNoiseFloor >> 8) & 0xFFu);
  
  // Byte 6: summary_counter
  payload[6] = sensorSummaryCounter++;
  
  // Byte 7: summary_flags (bit 0 = sensor active)
  payload[7] = 0x01; // Always active
  
  peakSinceLastReport = 0; // Reset peak for next report interval

  const INT32U canId = buildCanId(CAN_PRIORITY_CYCLIC, CAN_MSG_SENSOR_SUMMARY, NODE_ID);
  const INT8U txResult = canController.sendMsgBuf(canId, 0, 8, payload);
  if (txResult != CAN_OK) {
    Serial.print(F("CAN SENSOR_SUMMARY tx_fail code="));
    Serial.println(txResult);
  }
}

void sendCanResult(uint8_t requestMsgType, uint8_t requestId, uint8_t status, uint8_t data0, uint8_t data1) {
  if (!canNormalModeActive) {
    return;
  }

  INT8U payload[5] = {requestMsgType, requestId, status, data0, data1};
  const INT32U canId = buildCanId(CAN_PRIORITY_COMMAND, CAN_MSG_RESULT, NODE_ID);
  const INT8U txResult = canController.sendMsgBuf(canId, 0, 5, payload);
  if (txResult != CAN_OK) {
    Serial.print(F("CAN RESULT tx_fail code="));
    Serial.println(txResult);
  }
}

void handleCanCommand(uint8_t msgType, const INT8U* payload, INT8U payloadLen) {
  const uint8_t requestId = payloadLen >= 1 ? payload[0] : 0;

  if (msgType == CAN_MSG_SET_THRESHOLD) {
    if (payloadLen < 3) {
      sendCanResult(CAN_MSG_SET_THRESHOLD, requestId, CAN_RESULT_INVALID_ARG, 0, 0);
      return;
    }

    const int requestedThreshold = static_cast<int>(payload[1]) |
                                   (static_cast<int>(payload[2]) << 8);
    if (requestedThreshold < 0 || requestedThreshold > 1023) {
      sendCanResult(CAN_MSG_SET_THRESHOLD, requestId, CAN_RESULT_INVALID_ARG, payload[1], payload[2]);
      return;
    }

    hitThreshold = requestedThreshold;
    Serial.print(F("CAN CMD threshold="));
    Serial.println(hitThreshold);
    sendCanResult(CAN_MSG_SET_THRESHOLD, requestId, CAN_RESULT_OK,
                  static_cast<uint8_t>(hitThreshold & 0xFF),
                  static_cast<uint8_t>((hitThreshold >> 8) & 0xFF));
    return;
  }

  if (msgType == CAN_MSG_FLAP_COMMAND) {
    if (payloadLen < 2) {
      sendCanResult(CAN_MSG_FLAP_COMMAND, requestId, CAN_RESULT_INVALID_ARG, 0, 0);
      return;
    }

    const uint8_t action = payload[1];
    if (action == CAN_FLAP_UP) {
      setFlapState(true);
      autoModeEnabled = false;
      Serial.println(F("CAN CMD flap=up"));
      sendCanResult(CAN_MSG_FLAP_COMMAND, requestId, CAN_RESULT_OK, action, flapIsUp ? 1 : 0);
      return;
    }

    if (action == CAN_FLAP_DOWN) {
      setFlapState(false);
      autoModeEnabled = false;
      Serial.println(F("CAN CMD flap=down"));
      sendCanResult(CAN_MSG_FLAP_COMMAND, requestId, CAN_RESULT_OK, action, flapIsUp ? 1 : 0);
      return;
    }

    if (action == CAN_FLAP_TOGGLE) {
      setFlapState(!flapIsUp);
      autoModeEnabled = false;
      Serial.println(F("CAN CMD flap=toggle"));
      sendCanResult(CAN_MSG_FLAP_COMMAND, requestId, CAN_RESULT_OK, action, flapIsUp ? 1 : 0);
      return;
    }

    if (action == CAN_FLAP_AUTO) {
      autoModeEnabled = true;
      setFlapState(true);
      Serial.println(F("CAN CMD flap=auto"));
      sendCanResult(CAN_MSG_FLAP_COMMAND, requestId, CAN_RESULT_OK, action, flapIsUp ? 1 : 0);
      return;
    }

    if (action == CAN_FLAP_RESET) {
      eventCounter = 0;
      envelope = 0;
      lastHitAtMs = 0;
      autoModeEnabled = true;
      setFlapState(true);
      Serial.println(F("CAN CMD flap=reset"));
      sendCanResult(CAN_MSG_FLAP_COMMAND, requestId, CAN_RESULT_OK, action, flapIsUp ? 1 : 0);
      return;
    }

    sendCanResult(CAN_MSG_FLAP_COMMAND, requestId, CAN_RESULT_INVALID_ARG, action, 0);
    return;
  }

  sendCanResult(msgType, requestId, CAN_RESULT_UNSUPPORTED, 0, 0);
}

void pollCanFrames() {
  if (!canNormalModeActive) {
    return;
  }

  uint8_t processed = 0;
  while (processed < 4 && canController.checkReceive() == CAN_MSGAVAIL) {
    INT32U rxId = 0;
    INT8U rxLen = 0;
    INT8U rxData[8] = {0};
    const INT8U rxResult = canController.readMsgBuf(&rxId, &rxLen, rxData);
    if (rxResult != CAN_OK) {
      Serial.print(F("CAN RX FAIL code="));
      Serial.println(rxResult);
      break;
    }
    processed++;

    const bool isExtended = (rxId & CAN_IS_EXTENDED) != 0;
    const bool isRtr = (rxId & CAN_IS_REMOTE_REQUEST) != 0;
    if (isExtended || isRtr) {
      continue;
    }

    const uint16_t cleanId = static_cast<uint16_t>(rxId & 0x7FFu);
    const uint8_t msgType = static_cast<uint8_t>((cleanId >> 5) & 0x0Fu);
    const uint8_t nodeId = static_cast<uint8_t>(cleanId & 0x1Fu);
    if (nodeId != NODE_ID) {
      continue;
    }

    handleCanCommand(msgType, rxData, rxLen);
  }
}

void printBurnInProgress(unsigned long nowMs) {
  const unsigned long elapsed = nowMs - burnInStartedAtMs;
  if (elapsed >= SHORT_BURNIN_DURATION_MS) {
    if (lastBurnInLogAtMs != ULONG_MAX) {
      Serial.print(F("BURN-IN done elapsed_ms="));
      Serial.print(elapsed);
      Serial.print(F(" hb_sent="));
      Serial.print(heartbeatSentCount);
      Serial.print(F(" hb_err="));
      Serial.println(heartbeatErrorCount);
      lastBurnInLogAtMs = ULONG_MAX;
    }
    return;
  }

  if (nowMs - lastBurnInLogAtMs < SHORT_BURNIN_PROGRESS_MS) {
    return;
  }

  lastBurnInLogAtMs = nowMs;
  Serial.print(F("BURN-IN progress elapsed_ms="));
  Serial.print(elapsed);
  Serial.print(F(" remaining_ms="));
  Serial.print(SHORT_BURNIN_DURATION_MS - elapsed);
  Serial.print(F(" hb_sent="));
  Serial.print(heartbeatSentCount);
  Serial.print(F(" hb_err="));
  Serial.println(heartbeatErrorCount);
}

const __FlashStringHelper* canClockToText(INT8U clockSel) {
  if (clockSel == MCP_8MHZ) {
    return F("8MHz");
  }
  if (clockSel == MCP_16MHZ) {
    return F("16MHz");
  }
  if (clockSel == MCP_10MHZ) {
    return F("10MHz");
  }
  return F("unknown");
}

bool detectCanControllerClock() {
  struct ClockCandidate {
    INT8U clock;
    const __FlashStringHelper* label;
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
      Serial.print(F("CAN init OK with "));
      Serial.println(candidates[i].label);
      return true;
    }

    Serial.print(F("CAN init failed code="));
    Serial.println(initResult);
  }

  return false;
}

bool runCanControllerSelfTest() {
  canControllerDetected = false;
  canLoopbackPassed = false;
  canNormalModeActive = false;

  if (!detectCanControllerClock()) {
    Serial.println(F("CAN SELFTEST FAIL: no SPI communication with MCP2515"));
    return false;
  }

  canControllerDetected = true;

  if (canController.setMode(MCP_LOOPBACK) != CAN_OK) {
    Serial.println(F("CAN SELFTEST FAIL: cannot switch to loopback mode"));
    return false;
  }

  const INT8U txData[2] = {0xA5, 0x5A};
  const INT32U txId = 0x123;

  if (canController.sendMsgBuf(txId, 0, 2, const_cast<INT8U*>(txData)) != CAN_OK) {
    Serial.println(F("CAN SELFTEST FAIL: loopback send failed"));
    return false;
  }

  const unsigned long startMs = millis();
  while (millis() - startMs < 100) {
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

void setRelay(bool enabled) {
  digitalWrite(RELAY_PIN, enabled == RELAY_ACTIVE_HIGH ? HIGH : LOW);
}

void setFlapState(bool isUp) {
  flapIsUp = isUp;
  // Keep relay energized only while flap is down.
  setRelay(!flapIsUp);
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  t = toggle flap up/down"));
  Serial.println(F("  d = toggle periodic debug stream"));
  Serial.println(F("  c = run MCP2515 self test (no bus required)"));
  Serial.println(F("  s=<0..1023> = set hit threshold"));
  Serial.println(F("  ? = print this help"));
}

void printSample(unsigned long nowMs) {
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
  Serial.print(F(" noise_floor="));
  Serial.print(lastNoiseFloor);
  Serial.print(F(" threshold="));
  Serial.print(hitThreshold);
  Serial.print(F(" event_counter="));
  Serial.print(eventCounter);
  Serial.print(F(" relay="));
  Serial.print(flapIsUp ? 0 : 1);
  Serial.print(F(" flap_up="));
  Serial.print(flapIsUp ? 1 : 0);
  Serial.print(F(" can_detected="));
  Serial.print(canControllerDetected ? 1 : 0);
  Serial.print(F(" can_loopback="));
  Serial.print(canLoopbackPassed ? 1 : 0);
  Serial.print(F(" can_normal="));
  Serial.print(canNormalModeActive ? 1 : 0);
  Serial.print(F(" hb_sent="));
  Serial.print(heartbeatSentCount);
  Serial.print(F(" hb_err="));
  Serial.println(heartbeatErrorCount);
}

bool setThresholdValue(int value) {
  if (value < 0 || value > 1023) {
    Serial.println(F("CMD threshold invalid (expected 0..1023)"));
    return false;
  }

  hitThreshold = value;
  Serial.print(F("CMD threshold="));
  Serial.println(hitThreshold);
  return true;
}

void processSerialCommand(const char* command) {
  if (command[0] == '\0') {
    return;
  }

  if ((command[0] == 's' || command[0] == 'S') && command[1] == '=') {
    const long parsed = strtol(command + 2, nullptr, 10);
    setThresholdValue(static_cast<int>(parsed));
    return;
  }

  const char c = command[0];
  if (c == 't' || c == 'T') {
    setFlapState(!flapIsUp);
    autoModeEnabled = false;
    Serial.print(F("CMD flap_up="));
    Serial.println(flapIsUp ? 1 : 0);
  } else if (c == 'd' || c == 'D') {
    debugStreamingEnabled = !debugStreamingEnabled;
    Serial.print(F("CMD debug_stream="));
    Serial.println(debugStreamingEnabled ? F("on") : F("off"));
  } else if (c == 'c' || c == 'C') {
    Serial.println(F("CMD can_selftest"));
    if (runCanControllerSelfTest()) {
      switchToCanNormalMode();
    }
  } else if (c == '?') {
    printHelp();
  }
}

void handleSerial() {
  static char buffer[32];
  static uint8_t len = 0;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());

    if (len == 0 && (c == 't' || c == 'T' || c == 'd' || c == 'D' || c == 'c' || c == 'C' || c == '?')) {
      buffer[0] = c;
      buffer[1] = '\0';
      processSerialCommand(buffer);
      continue;
    }

    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buffer[len] = '\0';
        processSerialCommand(buffer);
        len = 0;
      }
      continue;
    }

    if (len < sizeof(buffer) - 1) {
      buffer[len++] = c;
    }
  }
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(MCP2515_INT_PIN, INPUT_PULLUP);
  setFlapState(true);

  Serial.begin(115200);
  while (!Serial) {
    // Exits immediately on ATmega328P boards without native USB.
  }

  baselineQ8 = static_cast<long>(analogRead(SENSOR_PIN)) << 8;
  noiseFloorQ8 = 0;
  bootMs = millis();
  burnInStartedAtMs = bootMs;
  lastBurnInLogAtMs = bootMs;

  Serial.println(F("TLP-ATmega328P Phase B: sensor bring-up"));
  Serial.print(F("Node ID: 0x"));
  Serial.println(NODE_ID, HEX);
  Serial.print(F("CAN prep: cs="));
  Serial.print(MCP2515_CS_PIN);
  Serial.print(F(" int="));
  Serial.print(MCP2515_INT_PIN);
  Serial.print(F(" bitrate="));
  Serial.println(CAN_BITRATE);

  if (runCanControllerSelfTest()) {
    switchToCanNormalMode();
  }
  Serial.println(F("Short burn-in window: 10 minutes"));
  printHelp();
}

void loop() {
  const unsigned long nowMs = millis();

  handleSerial();
  pollCanFrames();

  lastRaw = analogRead(SENSOR_PIN);

  baselineQ8 += ((static_cast<long>(lastRaw) << 8) - baselineQ8) >> BASELINE_FILTER_SHIFT;
  lastBaseline = static_cast<int>(baselineQ8 >> 8);
  lastCentered = lastRaw - lastBaseline;

  const int magnitude = abs(lastCentered);

  noiseFloorQ8 += ((static_cast<long>(magnitude) << 8) - noiseFloorQ8) >> NOISE_FLOOR_FILTER_SHIFT;
  lastNoiseFloor = static_cast<int>(noiseFloorQ8 >> 8);

  if (magnitude > envelope) {
    envelope = magnitude;
  } else if (envelope > 0) {
    envelope -= max(1, envelope >> ENVELOPE_DECAY_SHIFT);
  }
  
  // Track peak for SENSOR_SUMMARY reporting
  if (magnitude > peakSinceLastReport) {
    peakSinceLastReport = magnitude;
  }

  if (debugStreamingEnabled && nowMs - lastDebugAtMs >= DEBUG_INTERVAL_MS) {
    lastDebugAtMs = nowMs;
    printSample(nowMs);
  }

  if (nowMs - lastHeartbeatAtMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatAtMs = nowMs;
    sendHeartbeat(nowMs);
  }
  
  if (nowMs - lastSensorSummaryAtMs >= HEARTBEAT_INTERVAL_MS) {
    lastSensorSummaryAtMs = nowMs;
    sendSensorSummary(nowMs);
  }

  printBurnInProgress(nowMs);

  const bool refractoryExpired = (lastHitAtMs == 0) || (nowMs - lastHitAtMs >= REFRACTORY_MS);
  if (autoModeEnabled && envelope >= hitThreshold && refractoryExpired) {
    lastHitAtMs = nowMs;
    eventCounter++;
    setFlapState(false);

    Serial.print(F("HIT ms="));
    Serial.print(nowMs);
    Serial.print(F(" peak="));
    Serial.print(envelope);
    Serial.print(F(" threshold="));
    Serial.print(hitThreshold);
    Serial.print(F(" event_counter="));
    Serial.println(eventCounter);
  }

}