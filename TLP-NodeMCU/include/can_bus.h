#pragma once

#include <Arduino.h>
#include <mcp_can.h>

// Lifecycle of a command waiting for CAN acknowledgement.
enum class PendingCommandState : uint8_t {
  Pending = 0,
  Ack = 1,
  Nack = 2,
  SendFailed = 3,
  Expired = 4,
};

// Last observed CAN result frame content.
struct CanCommandResult {
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

// Internal pending-command queue entry.
struct PendingCommand {
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

// Controller/runtime health counters exposed in status output.
extern bool canControllerDetected;
extern bool canLoopbackPassed;
extern bool canNormalModeActive;
extern uint32_t canRxFrameCount;
extern uint32_t canRxIgnoredCount;
extern uint32_t canResultFrameCount;
extern INT8U detectedCanClock;

// Human-readable status texts for diagnostics and JSON.
const __FlashStringHelper *pendingStateToText(PendingCommandState state);
const __FlashStringHelper *canClockToText(INT8U clockSel);

// Pending command queue management.
void cleanupExpiredPendingCommands(unsigned long nowMs);
PendingCommand *findPendingCommand(uint8_t requestId);
PendingCommand *reservePendingCommand(uint8_t targetNodeId, uint8_t msgType, unsigned long nowMs);

// Sends one command frame (requestId in byte 0, payload in byte 1..).
bool sendCanCommand(uint8_t targetNodeId,
                    uint8_t requestMsgType,
                    uint8_t requestId,
                    const INT8U *payload,
                    INT8U payloadLen);

// MCP2515 setup, frame polling and periodic diagnostics.
bool runCanControllerSelfTest();
bool switchToCanNormalMode();
void pollCanFrames();
void printCanStatus(unsigned long nowMs);
