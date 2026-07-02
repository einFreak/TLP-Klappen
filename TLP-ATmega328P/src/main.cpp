#include <Arduino.h>

const uint8_t SENSOR_PIN = A0;
const uint8_t RELAY_PIN = 7;
const bool RELAY_ACTIVE_HIGH = true;

const uint8_t NODE_ID = 0x11;
const uint8_t MCP2515_CS_PIN = 10;
const uint8_t MCP2515_INT_PIN = 2;
const uint32_t CAN_BITRATE = 125000UL;

const uint8_t BASELINE_FILTER_SHIFT = 4;
const uint8_t ENVELOPE_DECAY_SHIFT = 2;

const unsigned long DEBUG_INTERVAL_MS = 100;
const unsigned long RELAY_PULSE_MS = 120;
const unsigned long REFRACTORY_MS = 220;

int hitThreshold = 60;
long baselineQ8 = 0;
int envelope = 0;
int lastRaw = 0;
int lastBaseline = 0;
int lastCentered = 0;
uint8_t eventCounter = 0;

bool relayPulseActive = false;
bool debugStreamingEnabled = true;

unsigned long relayOffAtMs = 0;
unsigned long lastHitAtMs = 0;
unsigned long lastDebugAtMs = 0;

void setRelay(bool enabled) {
  digitalWrite(RELAY_PIN, enabled == RELAY_ACTIVE_HIGH ? HIGH : LOW);
}

void startRelayPulse(unsigned long nowMs) {
  relayPulseActive = true;
  relayOffAtMs = nowMs + RELAY_PULSE_MS;
  setRelay(true);
}

void stopRelayPulse() {
  relayPulseActive = false;
  setRelay(false);
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  t = manual relay pulse"));
  Serial.println(F("  d = toggle periodic debug stream"));
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
  Serial.print(F(" threshold="));
  Serial.print(hitThreshold);
  Serial.print(F(" event_counter="));
  Serial.print(eventCounter);
  Serial.print(F(" relay="));
  Serial.println(relayPulseActive ? 1 : 0);
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

void processSerialCommand(const char* command, unsigned long nowMs) {
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
    Serial.println(F("CMD manual relay pulse"));
    startRelayPulse(nowMs);
  } else if (c == 'd' || c == 'D') {
    debugStreamingEnabled = !debugStreamingEnabled;
    Serial.print(F("CMD debug_stream="));
    Serial.println(debugStreamingEnabled ? F("on") : F("off"));
  } else if (c == '?') {
    printHelp();
  }
}

void handleSerial(unsigned long nowMs) {
  static char buffer[32];
  static uint8_t len = 0;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());

    if (len == 0 && (c == 't' || c == 'T' || c == 'd' || c == 'D' || c == '?')) {
      buffer[0] = c;
      buffer[1] = '\0';
      processSerialCommand(buffer, nowMs);
      continue;
    }

    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buffer[len] = '\0';
        processSerialCommand(buffer, nowMs);
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
  stopRelayPulse();

  Serial.begin(9600);
  while (!Serial) {
    // Exits immediately on ATmega328P boards without native USB.
  }

  baselineQ8 = static_cast<long>(analogRead(SENSOR_PIN)) << 8;

  Serial.println(F("TLP-ATmega328P Phase B: sensor bring-up"));
  Serial.print(F("Node ID: 0x"));
  Serial.println(NODE_ID, HEX);
  Serial.print(F("CAN prep: cs="));
  Serial.print(MCP2515_CS_PIN);
  Serial.print(F(" int="));
  Serial.print(MCP2515_INT_PIN);
  Serial.print(F(" bitrate="));
  Serial.println(CAN_BITRATE);
  printHelp();
}

void loop() {
  const unsigned long nowMs = millis();

  handleSerial(nowMs);

  lastRaw = analogRead(SENSOR_PIN);

  baselineQ8 += ((static_cast<long>(lastRaw) << 8) - baselineQ8) >> BASELINE_FILTER_SHIFT;
  lastBaseline = static_cast<int>(baselineQ8 >> 8);
  lastCentered = lastRaw - lastBaseline;

  const int magnitude = abs(lastCentered);
  if (magnitude > envelope) {
    envelope = magnitude;
  } else if (envelope > 0) {
    envelope -= max(1, envelope >> ENVELOPE_DECAY_SHIFT);
  }

  if (debugStreamingEnabled && nowMs - lastDebugAtMs >= DEBUG_INTERVAL_MS) {
    lastDebugAtMs = nowMs;
    printSample(nowMs);
  }

  const bool refractoryExpired = (lastHitAtMs == 0) || (nowMs - lastHitAtMs >= REFRACTORY_MS);
  if (envelope >= hitThreshold && refractoryExpired) {
    lastHitAtMs = nowMs;
    eventCounter++;
    startRelayPulse(nowMs);

    Serial.print(F("HIT ms="));
    Serial.print(nowMs);
    Serial.print(F(" peak="));
    Serial.print(envelope);
    Serial.print(F(" threshold="));
    Serial.print(hitThreshold);
    Serial.print(F(" event_counter="));
    Serial.println(eventCounter);
  }

  if (relayPulseActive && static_cast<long>(nowMs - relayOffAtMs) >= 0) {
    stopRelayPulse();
    Serial.print(F("RELAY off ms="));
    Serial.println(nowMs);
  }
}