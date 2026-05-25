/*
  TLP-Klappen - Phase A single node bring-up (ESP8266)

  Wiring assumptions for first hardware tests (NodeMCU v2):
  - Piezo / vibration sensor analog output -> A0
  - Sensor VCC/GND -> board VCC/GND
  - Relay control input -> D7 (GPIO13)

  Safety note:
  - Do NOT drive a bare relay coil directly from the ESP8266 pin.
  - Use a relay module with transistor/driver stage, or add an external
    transistor/MOSFET plus flyback diode across the coil.
  - Share ground between the ESP8266 and the relay driver stage.
*/

#include <Arduino.h>

const uint8_t SENSOR_PIN = A0;
const uint8_t RELAY_PIN = D7;
const bool RELAY_ACTIVE_HIGH = true;
const uint8_t ONBOARD_LED_PIN = LED_BUILTIN;
const bool ONBOARD_LED_ACTIVE_LOW = true;

const int HIT_THRESHOLD = 60;                // Envelope threshold to move flap up
const int RELEASE_THRESHOLD = 35;            // Lower threshold to move flap down
const unsigned long DEBUG_INTERVAL_MS = 50; // Periodic debug output
const unsigned long TELEPLOT_INTERVAL_MS = 50; // Periodic TelePlot output
const unsigned long MANUAL_OVERRIDE_MS = 10000; // Hold manual control after serial flap command

const uint8_t BASELINE_FILTER_SHIFT = 4; // 1/16 IIR baseline smoothing
const uint8_t ENVELOPE_DECAY_SHIFT = 2;  // Faster decay = smaller value

long baselineQ8 = 0;
int envelope = 0;
int lastRaw = 0;
int lastBaseline = 0;
int lastCentered = 0;
int hitThreshold = HIT_THRESHOLD;

bool flapIsUp = false;
bool debugStreamingEnabled = true;
bool teleplotStreamingEnabled = true;
bool manualOverrideEnabled = false;
bool hitLatched = false;

unsigned long lastDebugAtMs = 0;
unsigned long lastTeleplotAtMs = 0;
unsigned long manualOverrideUntilMs = 0;

void setFlap(bool moveUp)
{
  flapIsUp = moveUp;
  digitalWrite(RELAY_PIN, moveUp == RELAY_ACTIVE_HIGH ? HIGH : LOW);
  digitalWrite(ONBOARD_LED_PIN, moveUp == ONBOARD_LED_ACTIVE_LOW ? LOW : HIGH);
}

void printHelp()
{
  Serial.println(F("Commands:"));
  Serial.println(F("  u = move flap up (HIGH)"));
  Serial.println(F("  l = move flap down (LOW)"));
  Serial.println(F("  t = toggle flap state"));
  Serial.println(F("  a = return to automatic hit/release mode now"));
  Serial.println(F("  r = reset latched hit and flap down"));
  Serial.println(F("  d = toggle periodic debug stream"));
  Serial.println(F("  p = toggle TelePlot stream"));
  Serial.println(F("  s=<0..1023> = set hit threshold"));
  Serial.println(F("  ? = print this help"));
}

void printTeleplot(unsigned long nowMs)
{
  // TelePlot serial format on Serial ports requires a leading '>' prefix.
  // Frame format: name:timestamp:value[§unit|flags]
  Serial.print(F(">sensor_raw:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(lastRaw);

  Serial.print(F(">sensor_envelope:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(envelope);

  Serial.print(F(">flap_state:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(flapIsUp ? 1 : 0);

  Serial.print(F(">hitThreshold:"));
  Serial.print(nowMs);
  Serial.print(':');
  Serial.println(hitThreshold);
}

void printSample(unsigned long nowMs)
{
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
  Serial.print(F(" latched="));
  Serial.print(hitLatched ? 1 : 0);
  Serial.print(F(" mode="));
  Serial.print(manualOverrideEnabled ? F("manual") : F("auto"));
  Serial.print(F(" flap="));
  Serial.println(flapIsUp ? 1 : 0);
}

void processSerialCommand(const char *command)
{
  const unsigned long nowMs = millis();

  if (command[0] == '\0')
  {
    return;
  }

  if ((command[0] == 's' || command[0] == 'S') && command[1] == '=')
  {
    const long parsed = strtol(command + 2, nullptr, 10);
    if (parsed < 0 || parsed > 1023)
    {
      Serial.println(F("CMD threshold invalid (expected 0..1023)"));
      return;
    }

    hitThreshold = static_cast<int>(parsed);
    Serial.print(F("CMD threshold="));
    Serial.println(hitThreshold);
    return;
  }

  const char single = command[0];
  if (single == 'u' || single == 'U')
  {
    setFlap(true);
    manualOverrideEnabled = true;
    manualOverrideUntilMs = nowMs + MANUAL_OVERRIDE_MS;
    Serial.print(F("CMD flap=up (HIGH), mode=manual for ms="));
    Serial.println(MANUAL_OVERRIDE_MS);
  }
  else if (single == 'l' || single == 'L')
  {
    setFlap(false);
    manualOverrideEnabled = true;
    manualOverrideUntilMs = nowMs + MANUAL_OVERRIDE_MS;
    Serial.print(F("CMD flap=down (LOW), mode=manual for ms="));
    Serial.println(MANUAL_OVERRIDE_MS);
  }
  else if (single == 't' || single == 'T')
  {
    setFlap(!flapIsUp);
    manualOverrideEnabled = true;
    manualOverrideUntilMs = nowMs + MANUAL_OVERRIDE_MS;
    Serial.print(F("CMD flap="));
    Serial.print(flapIsUp ? F("up (HIGH)") : F("down (LOW)"));
    Serial.print(F(", mode=manual for ms="));
    Serial.println(MANUAL_OVERRIDE_MS);
  }
  else if (single == 'a' || single == 'A')
  {
    manualOverrideEnabled = false;
    manualOverrideUntilMs = 0;
    Serial.println(F("CMD mode=auto"));
  }
  else if (single == 'r' || single == 'R')
  {
    hitLatched = false;
    setFlap(false);
    Serial.println(F("CMD reset (latched=0, flap=down)"));
  }
  else if (single == 'd' || single == 'D')
  {
    debugStreamingEnabled = !debugStreamingEnabled;
    Serial.print(F("CMD debug_stream="));
    Serial.println(debugStreamingEnabled ? F("on") : F("off"));
  }
  else if (single == 'p' || single == 'P')
  {
    teleplotStreamingEnabled = !teleplotStreamingEnabled;
    Serial.print(F("CMD teleplot_stream="));
    Serial.println(teleplotStreamingEnabled ? F("on") : F("off"));
  }
  else if (single == '?')
  {
    printHelp();
  }
}

void handleSerial()
{
  static char commandBuffer[32];
  static uint8_t commandLength = 0;

  while (Serial.available() > 0)
  {
    const char c = static_cast<char>(Serial.read());

    if (c == '\n' || c == '\r')
    {
      if (commandLength > 0)
      {
        commandBuffer[commandLength] = '\0';
        processSerialCommand(commandBuffer);
        commandLength = 0;
      }
      continue;
    }

    if (commandLength < sizeof(commandBuffer) - 1)
    {
      commandBuffer[commandLength++] = c;
    }

    // Backward compatibility for one-letter commands sent without line ending.
    if (commandLength == 1 && (c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 't' || c == 'T' || c == 'a' || c == 'A' || c == 'r' || c == 'R' || c == 'd' || c == 'D' || c == 'p' || c == 'P' || c == '?'))
    {
      commandBuffer[1] = '\0';
      processSerialCommand(commandBuffer);
      commandLength = 0;
    }
  }
}

void setup()
{
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  setFlap(false);

  Serial.begin(115200);

  baselineQ8 = static_cast<long>(analogRead(SENSOR_PIN)) << 8;

  Serial.println();
  Serial.println(F("TLP-Klappen Phase A single-node test (ESP8266)"));
  Serial.println(F("Board: NodeMCU / ESP8266"));
  Serial.println(F("Sensor: analog vibration input on A0"));
  Serial.println(F("Relay: direct level control on D7 (GPIO13)"));
  Serial.println(F("HIGH = flap up, LOW = flap down"));
  Serial.println(F("Use serial commands: u/l/t (manual), a (auto), d = debug, p = teleplot, ? = help"));
  printHelp();
}

void loop()
{
  const unsigned long nowMs = millis();

  handleSerial();

  if (manualOverrideEnabled && nowMs >= manualOverrideUntilMs)
  {
    manualOverrideEnabled = false;
    manualOverrideUntilMs = 0;
    Serial.println(F("MODE auto (manual override timeout)"));
  }

  lastRaw = analogRead(SENSOR_PIN);

  // Slow baseline tracking (IIR): follows sensor drift, but not fast shocks/hits.
  baselineQ8 += ((static_cast<long>(lastRaw) << 8) - baselineQ8) >> BASELINE_FILTER_SHIFT;
  lastBaseline = static_cast<int>(baselineQ8 >> 8);
  // AC-like signal component around the moving baseline.
  lastCentered = lastRaw - lastBaseline;

  // Envelope follower: rise immediately on peaks, then decay smoothly over time.
  const int magnitude = abs(lastCentered);
  if (magnitude > envelope)
  {
    envelope = magnitude;
  }
  else if (envelope > 0)
  {
    envelope -= max(1, envelope >> ENVELOPE_DECAY_SHIFT);
  }

  if (debugStreamingEnabled && nowMs - lastDebugAtMs >= DEBUG_INTERVAL_MS)
  {
    lastDebugAtMs = nowMs;
    printSample(nowMs);
  }

  if (teleplotStreamingEnabled && nowMs - lastTeleplotAtMs >= TELEPLOT_INTERVAL_MS)
  {
    lastTeleplotAtMs = nowMs;
    printTeleplot(nowMs);
  }

  if (!manualOverrideEnabled && !hitLatched && envelope >= hitThreshold)
  {
    setFlap(true);
    hitLatched = true;

    Serial.print(F("HIT ms="));
    Serial.print(nowMs);
    Serial.print(F(" raw="));
    Serial.print(lastRaw);
    Serial.print(F(" centered="));
    Serial.print(lastCentered);
    Serial.print(F(" envelope="));
    Serial.print(envelope);
    Serial.print(F(" threshold="));
    Serial.println(hitThreshold);
  }
}
