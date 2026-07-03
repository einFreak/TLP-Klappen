#include "node_state.h"

NodeState nodeStates[Config::TLP_CAN_MAX_NODES] = {};

NodeState &getNodeState(uint8_t nodeId) {
  return nodeStates[nodeId & 0x1Fu];
}

bool isNodeOnline(const NodeState &state, unsigned long nowMs) {
  return state.seen && ((nowMs - state.lastHeartbeatAtMs) <= Config::NODE_HEARTBEAT_TIMEOUT_MS);
}

uint8_t countOnlineNodes(unsigned long nowMs) {
  uint8_t online = 0;
  for (uint8_t i = 0; i < Config::TLP_CAN_MAX_NODES; i++) {
    if (isNodeOnline(nodeStates[i], nowMs)) {
      online++;
    }
  }
  return online;
}
