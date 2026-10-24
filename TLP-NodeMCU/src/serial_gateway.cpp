#include "serial_gateway.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <cstring>
#include "config.h"
#include "control_api.h"
#include "status_output.h"
#include "web_server.h"

namespace {
bool localMode = false;
uint32_t session = 0;
uint32_t lastRequestId = 0;
unsigned long protocolAtMs = 0;
unsigned long pingAtMs = 0;
unsigned long rxAtMs = 0;
unsigned long telemetryAtMs = 0;
unsigned long centralAtMs = 0;
bool wifiRestorePending = false;
unsigned long wifiRestoreAtMs = 0;
char rxBuffer[128];
size_t rxLength = 0;
bool rxOverflow = false;
int snapshotNode = -3;
uint32_t generation = 0;
uint32_t nodeHashes[Config::TLP_CAN_MAX_NODES] = {};
uint32_t criticalHashes[Config::TLP_CAN_MAX_NODES] = {};
uint8_t nextNode = 0;

class FrameBuffer : public Print {
public:
  char bytes[512];
  size_t length = 0;
  size_t sent = 0;
  bool overflow = false;

  size_t write(uint8_t value) override {
    if (length >= sizeof(bytes)) {
      overflow = true;
      return 0;
    }
    bytes[length++] = static_cast<char>(value);
    return 1;
  }
  void begin(uint32_t id, const char *type) {
    length = sent = 0;
    overflow = false;
    print(F("TLP1 {\"id\":"));
    print(id);
    print(F(",\"session\":"));
    print(session);
    print(F(",\"type\":\""));
    print(type);
    print(F("\",\"data\":"));
  }
  void finish() {
    print(F("}\n"));
    if (overflow) length = 0;
  }
  bool busy() const { return sent < length; }
  void drain() {
    const size_t available = static_cast<size_t>(Serial.availableForWrite());
    const size_t count = min(min(available, length - sent), static_cast<size_t>(32));
    if (count) sent += Serial.write(reinterpret_cast<const uint8_t *>(bytes + sent), count);
  }
} tx;

void reply(uint32_t id, const char *type, const char *data) {
  tx.begin(id, type);
  tx.print(data);
  tx.finish();
}

void errorReply(uint32_t id, const char *error) {
  tx.begin(id, "error");
  tx.print(F("{\"error\":\""));
  tx.print(error);
  tx.print(F("\"}"));
  tx.finish();
}

bool restoreWifi() {
  localMode = false;
  maxLoopDurationUs = 0;
  snapshotNode = -3;
  WiFi.mode(WIFI_AP);
  const bool started = WiFi.softAP(Config::AP_SSID, Config::AP_PASSWORD,
    Config::AP_CHANNEL, Config::AP_HIDDEN, Config::AP_MAX_CLIENTS);
  wifiRestorePending = !started;
  wifiRestoreAtMs = millis();
  if (started) startWebServer();
  return started;
}

uint32_t hashBytes(const void *source, size_t length, uint32_t hash = 2166136261u) {
  const uint8_t *bytes = static_cast<const uint8_t *>(source);
  for (size_t index = 0; index < length; index++) hash = (hash ^ bytes[index]) * 16777619u;
  return hash;
}

uint32_t criticalHash(const NodeState &state) {
  uint32_t hash = hashBytes(&state.statusFlags, sizeof(state.statusFlags));
  hash = hashBytes(&state.lastFlapTimingAtMs, sizeof(state.lastFlapTimingAtMs), hash);
  hash = hashBytes(&state.rebootCount, sizeof(state.rebootCount), hash);
  return hash ^ static_cast<uint32_t>(isNodeOnline(state, millis()));
}

uint32_t nodeHash(const NodeState &state) {
  return hashBytes(&state, sizeof(state)) ^ static_cast<uint32_t>(isNodeOnline(state, millis()));
}

void sendNode(uint8_t nodeId) {
  const NodeState &state = nodeStates[nodeId];
  tx.begin(0, "node");
  writeNodeStatusJson(tx, state);
  tx.finish();
  nodeHashes[nodeId] = nodeHash(state);
  criticalHashes[nodeId] = criticalHash(state);
}

void processLine() {
  SerialRequest request = {};
  if (!parseSerialRequest(rxBuffer, request)) return;
  const unsigned long id = request.id;
  const unsigned long incomingSession = request.session;
  const char *operation = request.operation;
  const size_t count = request.argumentCount;
  if (strcmp(operation, "HELLO") == 0 && count == 0 && incomingSession == 0) {
    if (localMode) { errorReply(id, "busy"); return; }
    session = ESP.getCycleCount() | 1u;
    lastRequestId = id;
    protocolAtMs = millis();
    reply(id, "hello", "{\"device\":\"TLP-NodeMCU\",\"version\":1,\"mode\":\"wifi\",\"leaseMs\":10000}");
    return;
  }
  if (session == 0 || incomingSession != session ||
      (!localMode && millis() - protocolAtMs > Config::SERIAL_SESSION_MS)) {
    errorReply(id, "invalid session");
    return;
  }
  if (id <= lastRequestId) { errorReply(id, "duplicate request"); return; }
  lastRequestId = id;
  protocolAtMs = millis();
  if (strcmp(operation, "ENTER_LOCAL") == 0 && count == 0 && !localMode) {
    localMode = true;
    wifiRestorePending = false;
    stopWebServer();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    if (WiFi.getMode() != WIFI_OFF) {
      restoreWifi();
      errorReply(id, "wifi shutdown failed");
      return;
    }
    pingAtMs = millis();
    maxLoopDurationUs = 0;
    snapshotNode = -2;
    generation++;
    reply(id, "mode", "{\"mode\":\"local\"}");
    return;
  }
  if (!localMode) { errorReply(id, "not local"); return; }
  if (strcmp(operation, "PING") == 0 && count == 0) {
    pingAtMs = millis();
    reply(id, "pong", "{\"ok\":1}");
  } else if (strcmp(operation, "EXIT_LOCAL") == 0 && count == 0) {
    const bool started = restoreWifi();
    reply(id, "mode", started ? "{\"mode\":\"wifi\"}" : "{\"mode\":\"error\"}");
  } else if (strcmp(operation, "STATUS") == 0 && count == 0) {
    if (snapshotNode == -3) { snapshotNode = -2; generation++; }
    reply(id, "ok", "{\"ok\":1}");
  } else if (strcmp(operation, "RESULT") == 0 && count == 1) {
    unsigned long requestId;
    if (!parseUnsignedValue(request.arguments[0], 255, requestId) || requestId == 0) {
      errorReply(id, "invalid request id");
      return;
    }
    PendingCommand *pending = findPendingCommand(requestId);
    if (!pending) { errorReply(id, "unknown request"); return; }
    tx.begin(id, "result");
    writeCommandResultJson(tx, *pending);
    tx.finish();
    if (pending->state != PendingCommandState::Pending) pending->used = false;
  } else if ((strcmp(operation, "FLAP") == 0 || strcmp(operation, "THRESHOLD") == 0) && count == 2) {
    unsigned long nodeId;
    if (!parseUnsignedValue(request.arguments[0], 31, nodeId)) { errorReply(id, "invalid node"); return; }
    const char *error = nullptr;
    PendingCommand *pending;
    if (strcmp(operation, "FLAP") == 0) {
      pending = submitFlapCommand(nodeId, request.arguments[1], error);
    } else {
      unsigned long threshold;
      if (!parseUnsignedValue(request.arguments[1], 1023, threshold)) { errorReply(id, "invalid threshold"); return; }
      pending = submitThresholdCommand(nodeId, threshold, error);
    }
    if (!pending) { errorReply(id, error); return; }
    tx.begin(id, "accepted");
    tx.print(F("{\"pending\":1,\"requestId\":"));
    tx.print(pending->requestId);
    tx.print(F("}"));
    tx.finish();
  } else {
    errorReply(id, "invalid command");
  }
}

void streamStatus() {
  const unsigned long nowMs = millis();
  if (snapshotNode == -2) {
    tx.begin(0, "snapshot_begin");
    tx.print(F("{\"generation\":")); tx.print(generation); tx.print('}'); tx.finish();
    snapshotNode = -1;
  } else if (snapshotNode == -1) {
    tx.begin(0, "central"); writeCentralStatusJson(tx, true); tx.finish();
    centralAtMs = nowMs;
    snapshotNode = 0;
  } else if (snapshotNode >= 0) {
    while (snapshotNode < Config::TLP_CAN_MAX_NODES && !nodeStates[snapshotNode].seen) snapshotNode++;
    if (snapshotNode < Config::TLP_CAN_MAX_NODES) {
      sendNode(snapshotNode++);
    } else {
      tx.begin(0, "snapshot_end");
      tx.print(F("{\"generation\":")); tx.print(generation); tx.print('}'); tx.finish();
      snapshotNode = -3;
    }
  } else if (nowMs - telemetryAtMs >= 100) {
    telemetryAtMs = nowMs;
    for (uint8_t offset = 0; offset < Config::TLP_CAN_MAX_NODES; offset++) {
      const uint8_t nodeId = (nextNode + offset) % Config::TLP_CAN_MAX_NODES;
      if (nodeStates[nodeId].seen && criticalHashes[nodeId] != criticalHash(nodeStates[nodeId])) {
        sendNode(nodeId);
        nextNode = (nodeId + 1) % Config::TLP_CAN_MAX_NODES;
        return;
      }
    }
    for (uint8_t offset = 0; offset < Config::TLP_CAN_MAX_NODES; offset++) {
      const uint8_t nodeId = (nextNode + offset) % Config::TLP_CAN_MAX_NODES;
      if (nodeStates[nodeId].seen && nodeHashes[nodeId] != nodeHash(nodeStates[nodeId])) {
        sendNode(nodeId);
        nextNode = (nodeId + 1) % Config::TLP_CAN_MAX_NODES;
        return;
      }
    }
  }
  if (!tx.busy() && nowMs - centralAtMs >= 2000) {
    centralAtMs = nowMs;
    tx.begin(0, "central"); writeCentralStatusJson(tx, true); tx.finish();
  }
}
}

bool isLocalMode() { return localMode; }

bool serialProtocolActive() {
  return localMode || tx.busy() || (session != 0 && millis() - protocolAtMs <= Config::SERIAL_SESSION_MS);
}

void pollSerialGateway() {
  const unsigned long nowMs = millis();
  if (localMode && nowMs - pingAtMs >= Config::SERIAL_LEASE_MS) restoreWifi();
  if (wifiRestorePending && nowMs - wifiRestoreAtMs >= 2000) restoreWifi();
  tx.drain();
  if (tx.busy()) return;
  if (rxLength && nowMs - rxAtMs > 1000) { rxLength = 0; rxOverflow = true; }
  uint8_t budget = 64;
  while (budget-- && Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    rxAtMs = nowMs;
    if (value == '\n') {
      if (!rxOverflow) {
        rxBuffer[rxLength] = '\0';
        processLine();
      }
      rxLength = 0;
      rxOverflow = false;
      if (tx.busy()) return;
    } else if (!rxOverflow) {
      if (static_cast<unsigned char>(value) < 32 && value != '\r') rxOverflow = true;
      else if (rxLength < sizeof(rxBuffer) - 1) rxBuffer[rxLength++] = value;
      else rxOverflow = true;
    }
  }
  if (localMode && !tx.busy()) streamStatus();
}