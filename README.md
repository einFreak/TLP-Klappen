# TLP-Klappen

## Lokale USB-Oberflaeche

Mit [TLP-Local](TLP-Local/README.md) laeuft dieselbe Weboberflaeche auf dem PC und
kommuniziert in Edge/Chrome direkt per Web Serial mit dem NodeMCU. Start:
`cd TLP-Local` und `npm.cmd start` (Node.js 22+; kein npm install erforderlich).
Die ausgegebene localhost-URL oeffnen und **USB verbinden** waehlen.

Die aktuelle NodeMCU-Firmware und LittleFS muessen installiert sein. Standard
bleibt WLAN; erst ein bestaetigter serieller Befehl schaltet WLAN/HTTP/SSE aus.
Trennen oder 10 Sekunden ohne Lebenszeichen aktivieren WLAN wieder. ATmega und
CAN-Protokoll bleiben unveraendert. Details, Grenzen und Tests: [Anleitung](TLP-Local/README.md).

## Projektplanung: Klappscheiben für Airsoft mit Piezo-Sensorik

### Ziel
Klappscheiben sollen nicht nur durch Schusswaffen, sondern auch durch Airsoft-Kugeln zuverlässig auslösen.  
Dazu wird pro Klappscheibe ein Wattuino Pro Mini mit Piezo-Vibrationssensor eingesetzt. Bei erkanntem Treffer aktiviert der Wattuino über ein Relais die Klappe.

---

## 1) Technisches Zielbild

### Pro Klappscheibe (Node)
- **Mikrocontroller:** Wattuino Pro Mini
- **Sensor:** Piezo-Vibrationssensor (auf Klappscheibe verklebt)
- **Aktuator:** Relais-Ausgang zum Klappenantrieb (hoch/runter)
- **Kommunikation:** Bus-Anbindung an Zentrale

### Zentrale
- Empfängt Sensordaten und Klappenstatus aller Nodes
- Zeigt Werte im Dashboard
- Setzt pro Node Schwellwerte
- Sendet Konfigurations- und Steuerbefehle zurück

---

## 2) Empfehlung Bussystem (Entscheidungsvorschlag)

### Empfehlung: **CAN-Bus (mit MCP2515/TJA1050 je Node) + leichtes eigenes Protokoll**
Warum:
- robust gegen Störungen (Outdoor, lange Leitungen)
- echte Mehrmaster-Kommunikation (Senden + Empfangen für alle Teilnehmer)
- gute Skalierbarkeit bei vielen Nodes
- klare Nachrichten-IDs statt fester Reihenfolge in der Verkabelung

Hinweis:
- Nodes erhalten beim Flashen eine eindeutige `node_id`.
- Physikalisch wird ein gemeinsamer Bus mit Abschlusswiderständen an beiden Enden aufgebaut.
- Anwendungsschicht: **TLP-CAN/1**, siehe [`CAN_PROTOCOL.md`](./CAN_PROTOCOL.md)

**Alternative für schnellen Prototyp:** RS-485 + Modbus RTU (einfach, aber meist master-zentriert und weniger flexibel bei Broadcast/Event-Modellen).

---

## 3) Datenformat: Einzelwert vs. Array

### Empfehlung: **beides kombinieren**
1. **Zyklisch Einzelwert senden** (z. B. RMS/Peak alle 20–50 ms)  
   - geringe Buslast
   - gut für Live-Dashboard

2. **Bei Event zusätzlich kurzes Fenster senden** (Array, z. B. 32–64 Samples)  
   - hilfreich für Kalibrierung und Debugging
   - bessere Erkennung echter Treffer vs. Störvibrationen

Damit ist die Schwellwertfindung robust, ohne den Bus dauerhaft mit Rohdaten zu belasten.

---

## 4) Treffererkennung (Node-Logik)

Vorgeschlagene Pipeline:
1. Sensorwert einlesen (ADC)
2. Hochpass/Grundrausch-Unterdrückung (einfacher gleitender Mittelwert-Abzug)
3. Kurzzeit-Energie oder Peak berechnen
4. Mit konfigurierbarem Schwellwert vergleichen
5. Sperrzeit (`refractory_ms`, z. B. 150–300 ms), um Mehrfachtrigger zu vermeiden
6. Bei Treffer:
   - Relais auslösen (definierte Pulsdauer)
   - Event an Zentrale melden (inkl. Zeitstempel, Peak, optional Sample-Fenster)

---

## 5) Dashboard-Plattform

### Empfehlung:
- **Laptop/Raspberry Pi als Zentrale**, nicht ein weiterer kleiner Arduino für UI

Begründung:
- schnellere Entwicklung eines Dashboards
- bessere Datenhaltung/Logging
- einfache Kalibrier-UI pro Node

Pragmatischer Start:
- Python + Web-UI (z. B. FastAPI/Flask + kleines Frontend)
- CAN-USB-Adapter oder serielles Gateway zur Busanbindung

---

## 6) Minimales Nachrichtenmodell

- `HEARTBEAT`: node_id, firmware_version, uptime, klappenstatus
- `SENSOR_SUMMARY`: node_id, peak, rms, noise_floor
- `HIT_EVENT`: node_id, peak, threshold, timestamp, relay_fired
- `SENSOR_WINDOW` (optional): node_id, sample_count, samples[]
- `SET_THRESHOLD`: node_id, threshold
- `SET_PARAMS`: node_id, refractory_ms, relay_pulse_ms, report_interval_ms
- `CMD_FLAP`: node_id, action (UP/DOWN/TRIGGER)

Konkrete CAN-Identifier, Payload-Layouts, Ack-Regeln und Fragmentierung sind in [`CAN_PROTOCOL.md`](./CAN_PROTOCOL.md) festgelegt.

---

## 7) Umsetzungsphasen

1. **Phase A – Einzelner Node**
   - Sensor lesen, Trigger lokal testen
   - Relais sauber schalten
   - Serielle Debug-Ausgabe

2. **Phase B – Buskommunikation**
   - Heartbeat + Sensor Summary + Hit Event
   - Parametrierung von der Zentrale

3. **Phase C – Dashboard**
   - Live-Werte je Node
   - Schwellwerte einstellen/speichern
   - Trefferhistorie

4. **Phase D – Feldtest**
   - Kalibrierung mit verschiedenen Distanzen/Kugelgewichten
   - Fehltriggerquote reduzieren

---

## Phase A Firmware-Startpunkt

Für den lokalen Hardware-Test gibt es jetzt ein Arduino-Beispiel unter  
`arduino/phase_a_single_node/phase_a_single_node.ino`.

Ziel des Sketches:
- Analogwert des Vibrations-/Piezo-Sensors lesen
- einfachen Basislinien-/Envelope-Filter für lokale Treffererkennung nutzen
- Relais bei erkanntem Treffer mit sauberem Zeitpuls schalten
- Rohwert, gefilterte Werte und Treffer über `Serial` ausgeben

Kurz zur Nutzung:
- In der Arduino IDE als Board **Arduino Pro or Pro Mini** mit passendem ATmega328P-Profil für den verwendeten Wattuino auswählen
- Sensor-Analogausgang an `A0`, Relais-Steuersignal an `D7`
- `HIT_THRESHOLD`, `RELAY_PULSE_MS` und `REFRACTORY_MS` am Sketch-Anfang auf die Mechanik anpassen
- Seriellen Monitor auf `115200 Baud` öffnen; `t` löst einen manuellen Relaispuls aus, `d` schaltet den periodischen Debug-Stream um

Wichtig:
- Eine Relais-Spule nicht direkt am AVR-Pin treiben
- Relais nur über Transistor/MOSFET bzw. fertiges Relaismodul mit Freilaufdiode ansteuern
- Gemeinsame Masse zwischen Wattuino und Treiberstufe herstellen

## 8) Offene Punkte (für nächste Entscheidung)

- Spannungsversorgung/EMV-Konzept bei mehreren Klappen
- mechanische Sensorbefestigung (wiederholbar, wetterfest)
- Default-Kalibrierstrategie beim ersten Start neuer Nodes
