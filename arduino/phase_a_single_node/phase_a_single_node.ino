/*
  TLP-Klappen - Phase A single node bring-up

  Wiring assumptions for first hardware tests:
  - Wattuino Pro Mini / Arduino Pro Mini compatible board
  - Piezo / vibration sensor analog output -> A0
  - Sensor VCC/GND -> board VCC/GND
  - Relay control input -> D7

  Safety note:
  - Do NOT drive a bare relay coil directly from the AVR pin.
  - Use a relay module with transistor/driver stage, or add an external
    transistor/MOSFET plus flyback diode across the coil.
  - Share ground between the Pro Mini and the relay driver stage.
*/

const uint8_t SENSOR_PIN = A0;
const uint8_t RELAY_PIN = 7;
const bool RELAY_ACTIVE_HIGH = true;

const int HIT_THRESHOLD = 60;                    // Envelope threshold for a hit
const unsigned long RELAY_PULSE_MS = 120;       // Relay ON pulse per hit
const unsigned long REFRACTORY_MS = 220;        // Ignore retriggers for this time
const unsigned long DEBUG_INTERVAL_MS = 50;     // Periodic debug output

const uint8_t BASELINE_FILTER_SHIFT = 4;        // 1/16 IIR baseline smoothing
const uint8_t ENVELOPE_DECAY_SHIFT = 2;         // Faster decay = smaller value

long baselineQ8 = 0;
int envelope = 0;
int lastRaw = 0;
int lastBaseline = 0;
int lastCentered = 0;

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
  Serial.print(HIT_THRESHOLD);
  Serial.print(F(" relay="));
  Serial.println(relayPulseActive ? 1 : 0);
}

void handleSerial(unsigned long nowMs) {
  while (Serial.available() > 0) {
    char command = static_cast<char>(Serial.read());

    if (command == 't' || command == 'T') {
      Serial.println(F("CMD manual relay pulse"));
      startRelayPulse(nowMs);
    } else if (command == 'd' || command == 'D') {
      debugStreamingEnabled = !debugStreamingEnabled;
      Serial.print(F("CMD debug_stream="));
      Serial.println(debugStreamingEnabled ? F("on") : F("off"));
    } else if (command == '?') {
      printHelp();
    }
  }
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  stopRelayPulse();

  Serial.begin(115200);
  while (!Serial) {
    // Safe on boards without native USB; exits immediately on Pro Mini class hardware.
  }

  baselineQ8 = static_cast<long>(analogRead(SENSOR_PIN)) << 8;

  Serial.println(F("TLP-Klappen Phase A single-node test"));
  Serial.println(F("Board: Arduino Pro Mini / Wattuino Pro Mini compatible"));
  Serial.println(F("Sensor: analog vibration input on A0"));
  Serial.println(F("Relay: timed pulse output on D7"));
  Serial.println(F("Use serial commands: t = pulse relay, d = toggle debug, ? = help"));
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
  if (envelope >= HIT_THRESHOLD && refractoryExpired) {
    lastHitAtMs = nowMs;
    startRelayPulse(nowMs);

    Serial.print(F("HIT ms="));
    Serial.print(nowMs);
    Serial.print(F(" raw="));
    Serial.print(lastRaw);
    Serial.print(F(" centered="));
    Serial.print(lastCentered);
    Serial.print(F(" envelope="));
    Serial.print(envelope);
    Serial.print(F(" threshold="));
    Serial.println(HIT_THRESHOLD);
  }

  if (relayPulseActive && static_cast<long>(nowMs - relayOffAtMs) >= 0) {
    stopRelayPulse();
    Serial.print(F("RELAY off ms="));
    Serial.println(nowMs);
  }
}
