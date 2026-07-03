#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "config.h"

// Per-node telemetry cache aggregated from CAN frames.
struct NodeState {
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
  uint16_t currentRaw;
  uint16_t peak;
  uint16_t noiseFloor;
  uint8_t sensorSummaryCounter;
  unsigned long lastSensorSummaryAtMs;
  uint32_t lastUpDurationMs;
  uint8_t lastUpEventCounter;
  unsigned long lastFlapTimingAtMs;
};

// Fixed-size node table indexed by TLP node ID.
extern NodeState nodeStates[Config::TLP_CAN_MAX_NODES];

// Returns node slot by ID (ID is masked to valid range).
NodeState &getNodeState(uint8_t nodeId);
// True when node was seen recently enough by heartbeat timeout.
bool isNodeOnline(const NodeState &state, unsigned long nowMs);
// Number of nodes currently considered online.
uint8_t countOnlineNodes(unsigned long nowMs);
