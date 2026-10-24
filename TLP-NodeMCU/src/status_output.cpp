#include "status_output.h"

#include <ESP8266WiFi.h>
#include "config.h"

unsigned long maxLoopDurationUs = 0;

void writeCentralStatusJson(Print &out, bool local, bool closeObject) {
  out.print(F("{\"mode\":\""));
  out.print(local ? F("local") : F("wifi"));
  out.print(F("\",\"ssid\":\""));
  if (!local) out.print(Config::AP_SSID);
  out.print(F("\",\"ip\":\""));
  if (!local) out.print(WiFi.softAPIP());
  out.print(F("\",\"channel\":"));
  out.print(local ? 0 : WiFi.channel());
  out.print(F(",\"clients\":"));
  out.print(local ? 0 : WiFi.softAPgetStationNum());
  out.print(F(",\"mac\":\""));
  if (!local) out.print(WiFi.softAPmacAddress());
  out.print(F("\",\"canDetected\":"));
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
  out.print(countOnlineNodes(millis()));
  out.print(F(",\"canClockMHz\":"));
  out.print(detectedCanClock == MCP_8MHZ ? 8 : detectedCanClock == MCP_16MHZ ? 16 : detectedCanClock == MCP_10MHZ ? 10 : 0);
  out.print(F(",\"freeHeap\":"));
  out.print(ESP.getFreeHeap());
  out.print(F(",\"maxFreeBlock\":"));
  out.print(ESP.getMaxFreeBlockSize());
  out.print(F(",\"maxLoopUs\":"));
  out.print(maxLoopDurationUs);
  if (closeObject) out.print('}');
}

void writeNodeStatusJson(Print &out, const NodeState &state) {
  out.print(F("{\"id\":"));
  out.print(state.nodeId);
  out.print(F(",\"online\":"));
  out.print(isNodeOnline(state, millis()) ? 1 : 0);
  out.print(F(",\"hb\":"));
  out.print(state.heartbeatCount);
  out.print(F(",\"reboots\":"));
  out.print(state.rebootCount);
  out.print(F(",\"flags\":"));
  out.print(state.statusFlags);
  out.print(F(",\"uptimeSeconds\":"));
  out.print(state.uptimeSeconds);
  out.print(F(",\"sensorRaw\":"));
  out.print(state.currentRaw);
  out.print(F(",\"sensorPeak\":"));
  out.print(state.peak);
  out.print(F(",\"sensorNoise\":"));
  out.print(state.noiseFloor);
  out.print(F(",\"lastUpTimeMs\":"));
  out.print(state.lastUpDurationMs);
  out.print(F(",\"lastUpEventCounter\":"));
  out.print(state.lastUpEventCounter);
  out.print(F(",\"lastUpEventSequence\":"));
  out.print(state.lastUpEventSequence);
  out.print('}');
}

void writeStatusJson(Print &out, bool includeNodes) {
  writeCentralStatusJson(out, false, false);
  if (includeNodes) {
    out.print(F(",\"nodes\":["));
    bool first = true;
    for (const NodeState &state : nodeStates) {
      if (!state.seen) continue;
      if (!first) out.print(',');
      first = false;
      writeNodeStatusJson(out, state);
    }
    out.print(']');
  }
  out.print('}');
}

void writeCommandResultJson(Print &out, const PendingCommand &entry) {
  out.print(F("{\"requestId\":"));
  out.print(entry.requestId);
  out.print(F(",\"node\":"));
  out.print(entry.targetNodeId);
  out.print(F(",\"msgType\":"));
  out.print(entry.msgType);
  out.print(F(",\"state\":\""));
  out.print(pendingStateToText(entry.state));
  out.print(F("\",\"done\":"));
  out.print(entry.state == PendingCommandState::Pending ? 0 : 1);
  out.print(F(",\"status\":"));
  out.print(entry.resultStatus);
  out.print(F(",\"data0\":"));
  out.print(entry.data0);
  out.print(F(",\"data1\":"));
  out.print(entry.data1);
  out.print('}');
}