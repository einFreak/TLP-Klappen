#include "web_server.h"

#include <ESP8266WiFi.h>
#include <LittleFS.h>

#include "can_bus.h"
#include "config.h"
#include "node_state.h"
#include "system_state.h"

AsyncWebServer webServer(80);
AsyncEventSource webEvents("/events");

static bool parseNodeIdParam(AsyncWebServerRequest *request, uint8_t &nodeId) {
  String nodeValue;
  if (request->hasParam("node", true)) {
    nodeValue = request->getParam("node", true)->value();
  } else if (request->hasParam("node")) {
    nodeValue = request->getParam("node")->value();
  }

  if (nodeValue.length() == 0) {
    return false;
  }

  const long parsedNode = strtol(nodeValue.c_str(), nullptr, 10);
  if (parsedNode < 0 || parsedNode > 31) {
    return false;
  }

  nodeId = static_cast<uint8_t>(parsedNode);
  return true;
}

static void writeStatusJson(Print &out, bool includeExtendedNodes) {
  const unsigned long nowMs = millis();
  out.print(F("{\"ssid\":\""));
  out.print(Config::AP_SSID);
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

  if (includeExtendedNodes) {
    out.print(F(",\"nodes\":["));
    bool firstNode = true;
    for (uint8_t i = 0; i < Config::TLP_CAN_MAX_NODES; i++) {
      const NodeState &state = nodeStates[i];
      if (!state.seen) {
        continue;
      }

      if (!firstNode) {
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
      out.print(F("}"));
    }
    out.print(F("]"));
  }
  out.print(F("}"));
}

static String buildStatusJson(bool includeExtendedNodes) {
  String json;
  json.reserve(includeExtendedNodes ? Config::STATUS_JSON_RESERVE_FULL : Config::STATUS_JSON_RESERVE_COMPACT);
  const unsigned long nowMs = millis();
  json += F("{\"ssid\":\"");
  json += Config::AP_SSID;
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

  if (includeExtendedNodes) {
    json += F(",\"nodes\":[");
    bool firstNode = true;
    for (uint8_t i = 0; i < Config::TLP_CAN_MAX_NODES; i++) {
      const NodeState &state = nodeStates[i];
      if (!state.seen) {
        continue;
      }

      if (!firstNode) {
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
      json += F(",\"sensorRaw\":");
      json += state.currentRaw;
      json += F(",\"sensorPeak\":");
      json += state.peak;
      json += F(",\"sensorNoise\":");
      json += state.noiseFloor;
      json += F(",\"lastUpTimeMs\":");
      json += state.lastUpDurationMs;
      json += F(",\"lastUpEventCounter\":");
      json += state.lastUpEventCounter;
      json += F("}");
    }
    json += F("]");
  }
  json += F("}");
  return json;
}

static void sendStatusJsonResponse(AsyncWebServerRequest *request, bool includeExtendedNodes) {
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  writeStatusJson(*response, includeExtendedNodes);
  request->send(response);
}

static void sendCommandAcceptedResponse(AsyncWebServerRequest *request, const PendingCommand &entry) {
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->print(F("{\"ok\":1,\"pending\":1,\"requestId\":"));
  response->print(entry.requestId);
  response->print(F(",\"node\":"));
  response->print(entry.targetNodeId);
  response->print(F("}"));
  response->setCode(202);
  request->send(response);
}

static void sendCommandResultResponse(AsyncWebServerRequest *request, const PendingCommand &entry) {
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

void startWebServer() {
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendStatusJsonResponse(request, true);
  });

  webServer.on("/api/flap", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action;
    if      (request->hasParam("action", true)) action = request->getParam("action", true)->value();
    else if (request->hasParam("action"))       action = request->getParam("action")->value();
    action.toLowerCase();

    uint8_t flapCommand = Config::CAN_FLAP_TOGGLE;
    if      (action == "up")     flapCommand = Config::CAN_FLAP_UP;
    else if (action == "down")   flapCommand = Config::CAN_FLAP_DOWN;
    else if (action == "toggle") flapCommand = Config::CAN_FLAP_TOGGLE;
    else if (action == "auto")   flapCommand = Config::CAN_FLAP_AUTO;
    else if (action == "reset")  flapCommand = Config::CAN_FLAP_RESET;
    else { request->send(400, "application/json", "{\"error\":\"invalid action\"}"); return; }

    uint8_t targetNodeId = 0;
    if (!parseNodeIdParam(request, targetNodeId)) {
      request->send(400, "application/json", "{\"error\":\"missing or invalid node\"}");
      return;
    }

    const unsigned long nowMs = millis();
    PendingCommand *pending = reservePendingCommand(targetNodeId, Config::TLP_CAN_MSG_FLAP_COMMAND, nowMs);
    if (pending == nullptr) {
      request->send(503, "application/json", "{\"error\":\"pending queue full\"}");
      return;
    }

    INT8U payload[1] = {flapCommand};
    if (!sendCanCommand(targetNodeId, Config::TLP_CAN_MSG_FLAP_COMMAND, pending->requestId, payload, 1)) {
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
    if (threshold < 0 || threshold > 1023) {
      request->send(400, "application/json", "{\"error\":\"value out of range\"}");
      return;
    }

    uint8_t targetNodeId = 0;
    if (!parseNodeIdParam(request, targetNodeId)) {
      request->send(400, "application/json", "{\"error\":\"missing or invalid node\"}");
      return;
    }

    const unsigned long nowMs = millis();
    PendingCommand *pending = reservePendingCommand(targetNodeId, Config::TLP_CAN_MSG_SET_THRESHOLD, nowMs);
    if (pending == nullptr) {
      request->send(503, "application/json", "{\"error\":\"pending queue full\"}");
      return;
    }

    INT8U payload[2] = {
      static_cast<INT8U>(threshold & 0xFF),
      static_cast<INT8U>((threshold >> 8) & 0xFF)
    };

    if (!sendCanCommand(targetNodeId, Config::TLP_CAN_MSG_SET_THRESHOLD, pending->requestId, payload, 2)) {
      pending->state = PendingCommandState::SendFailed;
      pending->updatedAtMs = nowMs;
      request->send(503, "application/json", "{\"error\":\"can send failed\"}");
      return;
    }

    sendCommandAcceptedResponse(request, *pending);
  });

  webServer.on("/api/command-result", HTTP_GET, [](AsyncWebServerRequest *request) {
    String idValue;
    if (request->hasParam("id")) {
      idValue = request->getParam("id")->value();
    } else {
      request->send(400, "application/json", "{\"error\":\"missing id\"}");
      return;
    }

    const long parsedId = strtol(idValue.c_str(), nullptr, 10);
    if (parsedId < 1 || parsedId > 255) {
      request->send(400, "application/json", "{\"error\":\"invalid id\"}");
      return;
    }

    cleanupExpiredPendingCommands(millis());
    PendingCommand *entry = findPendingCommand(static_cast<uint8_t>(parsedId));
    if (entry == nullptr) {
      request->send(404, "application/json", "{\"error\":\"unknown request\"}");
      return;
    }

    sendCommandResultResponse(request, *entry);
  });

  webEvents.onConnect([](AsyncEventSourceClient *client) {
    client->send("connected", nullptr, millis());
  });

  webServer.addHandler(&webEvents);
  webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  webServer.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "application/json", "{\"error\":\"not found\"}");
  });

  webServer.begin();
  Serial.println(F("Web server started on port 80"));
}

void streamApStatus() {
  static unsigned long lastStreamAtMs = 0;
  static uint32_t sseDropped = 0;
  const unsigned long nowMs = millis();

  if (nowMs - lastStreamAtMs >= Config::SSE_STREAM_INTERVAL_MS) {
    lastStreamAtMs = nowMs;
    if (webEvents.count() == 0) {
      return;
    }

    const String status = buildStatusJson(true);
    const AsyncEventSource::SendStatus sendStatus = webEvents.send(status.c_str(), "status", nowMs);
    if (sendStatus != AsyncEventSource::SendStatus::ENQUEUED) {
      sseDropped++;
      if ((sseDropped % 10u) == 0u) {
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

void printApStatus() {
  Serial.print(F("AP mode="));
  Serial.print((WiFi.getMode() & WIFI_AP) ? F("on") : F("off"));
  Serial.print(F(", ssid='"));
  Serial.print(Config::AP_SSID);
  Serial.print(F("', ip="));
  Serial.print(WiFi.softAPIP());
  Serial.print(F(", channel="));
  Serial.print(WiFi.channel());
  Serial.print(F(", clients="));
  Serial.println(WiFi.softAPgetStationNum());
}
