#include "control_api.h"

#include <cstring>
#include "config.h"

static PendingCommand *submitCommand(uint8_t nodeId, uint8_t type, const INT8U *payload,
                                     uint8_t length, const char *&error) {
  error = nullptr;
  if (nodeId >= Config::TLP_CAN_MAX_NODES) {
    error = "invalid node";
    return nullptr;
  }
  const unsigned long nowMs = millis();
  PendingCommand *pending = reservePendingCommand(nodeId, type, nowMs);
  if (pending == nullptr) {
    error = "pending queue full";
    return nullptr;
  }
  if (!sendCanCommand(nodeId, type, pending->requestId, payload, length)) {
    pending->state = PendingCommandState::SendFailed;
    pending->updatedAtMs = nowMs;
    error = "can send failed";
    return nullptr;
  }
  return pending;
}

PendingCommand *submitFlapCommand(uint8_t nodeId, const char *action, const char *&error) {
  uint8_t command;
  if (strcmp(action, "up") == 0) command = Config::CAN_FLAP_UP;
  else if (strcmp(action, "down") == 0) command = Config::CAN_FLAP_DOWN;
  else if (strcmp(action, "toggle") == 0) command = Config::CAN_FLAP_TOGGLE;
  else if (strcmp(action, "auto") == 0) command = Config::CAN_FLAP_AUTO;
  else if (strcmp(action, "reset") == 0) command = Config::CAN_FLAP_RESET;
  else {
    error = "invalid action";
    return nullptr;
  }
  return submitCommand(nodeId, Config::TLP_CAN_MSG_FLAP_COMMAND, &command, 1, error);
}

PendingCommand *submitThresholdCommand(uint8_t nodeId, unsigned int value, const char *&error) {
  if (value > 1023) {
    error = "value out of range";
    return nullptr;
  }
  const INT8U payload[] = {static_cast<INT8U>(value & 0xFF), static_cast<INT8U>(value >> 8)};
  return submitCommand(nodeId, Config::TLP_CAN_MSG_SET_THRESHOLD, payload, 2, error);
}