#include "web_server.h"

#include <ESP8266WiFi.h>
#include <LittleFS.h>

#include "can_bus.h"
#include "config.h"
#include "control_api.h"
#include "node_state.h"
#include "status_output.h"
#include "serial_gateway.h"

AsyncWebServer webServer(80);
AsyncEventSource webEvents("/events");
static bool webRoutesRegistered = false;
static bool webRunning = false;

class StringPrintSink : public Print {
public:
  explicit StringPrintSink(String &target) : target_(target) {}

  size_t write(uint8_t c) override {
    target_ += static_cast<char>(c);
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      target_ += static_cast<char>(buffer[i]);
    }
    return size;
  }

private:
  String &target_;
};

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

  unsigned long parsedNode;
  if (!parseUnsignedValue(nodeValue.c_str(), 31, parsedNode)) {
    return false;
  }

  nodeId = static_cast<uint8_t>(parsedNode);
  return true;
}

static String buildStatusJson(bool includeExtendedNodes) {
  String json;
  json.reserve(includeExtendedNodes ? Config::STATUS_JSON_RESERVE_FULL : Config::STATUS_JSON_RESERVE_COMPACT);
  StringPrintSink sink(json);
  writeStatusJson(sink, includeExtendedNodes);
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
  writeCommandResultJson(*response, entry);
  request->send(response);
}

void startWebServer() {
  if (webRunning) return;
  if (webRoutesRegistered) {
    webServer.begin();
    webRunning = true;
    return;
  }
  webRoutesRegistered = true;
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendStatusJsonResponse(request, true);
  });

  webServer.on("/api/flap", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (isLocalMode()) { request->send(503, "application/json", "{\"error\":\"local mode\"}"); return; }
    String action;
    if      (request->hasParam("action", true)) action = request->getParam("action", true)->value();
    else if (request->hasParam("action"))       action = request->getParam("action")->value();
    action.toLowerCase();

    uint8_t targetNodeId = 0;
    if (!parseNodeIdParam(request, targetNodeId)) {
      request->send(400, "application/json", "{\"error\":\"missing or invalid node\"}");
      return;
    }

    const char *error = nullptr;
    PendingCommand *pending = submitFlapCommand(targetNodeId, action.c_str(), error);
    if (pending == nullptr) {
      const int code = strcmp(error, "invalid action") == 0 ? 400 : 503;
      request->send(code, "application/json", String("{\"error\":\"") + error + "\"}");
      return;
    }

    sendCommandAcceptedResponse(request, *pending);
  });

  webServer.on("/api/threshold", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (isLocalMode()) { request->send(503, "application/json", "{\"error\":\"local mode\"}"); return; }
    String val;
    if      (request->hasParam("value", true)) val = request->getParam("value", true)->value();
    else if (request->hasParam("value"))       val = request->getParam("value")->value();
    if (val.length() == 0) { request->send(400, "application/json", "{\"error\":\"missing value\"}"); return; }
    unsigned long threshold;
    if (!parseUnsignedValue(val.c_str(), 1023, threshold)) {
      request->send(400, "application/json", "{\"error\":\"value out of range\"}");
      return;
    }

    uint8_t targetNodeId = 0;
    if (!parseNodeIdParam(request, targetNodeId)) {
      request->send(400, "application/json", "{\"error\":\"missing or invalid node\"}");
      return;
    }

    const char *error = nullptr;
    PendingCommand *pending = submitThresholdCommand(targetNodeId, threshold, error);
    if (pending == nullptr) {
      request->send(503, "application/json", String("{\"error\":\"") + error + "\"}");
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
  webRunning = true;
  if (!serialProtocolActive()) Serial.println(F("Web server started on port 80"));
}

void stopWebServer() {
  webRunning = false;
  webEvents.close();
  webServer.end();
}

void streamApStatus() {
  if (!webRunning || isLocalMode()) return;
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
      if ((sseDropped % 10u) == 0u && !serialProtocolActive()) {
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
  if (serialProtocolActive()) return;
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
