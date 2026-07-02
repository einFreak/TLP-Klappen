# TLP-CAN/1 – leichtes Anwendungsprotokoll auf Classic CAN

## Ziel und Umfang

Für TLP-Klappen wird **CAN als Transport** und darüber ein **kleines projektspezifisches Anwendungsprotokoll** verwendet.
Die Spezifikation ist bewusst leichtgewichtig: **Classic CAN 2.0A mit 11-Bit-Identifiern**, feste Nachrichtenformate, keine komplexen Dienste wie CANopen.

Ziele:
- kleine Implementierung auf Wattuino Pro Mini + MCP2515
- klare Priorisierung für Trefferereignisse
- einfache Parametrierung durch eine Zentrale
- saubere Erweiterbarkeit ohne unnötigen Overhead

Annahmen:
- genau **eine aktive Zentrale** auf dem Bus
- mehrere Nodes mit identischer Grundfunktion
- Standard-Datenframes mit **0 bis 8 Datenbytes**

---

## 1) Architektur

- **Transport:** Classic CAN 2.0A, 11-Bit-Identifier, Standard-Frames
- **Bitrate:** projektspezifisch, für alle Teilnehmer identisch (z. B. 125 kbit/s oder 250 kbit/s)
- **Anwendungsschicht:** `TLP-CAN/1`
- **Topologie:** gemeinsamer CAN-Bus mit Abschluss an beiden Enden

Das Protokoll trennt klar:
- **CAN** = Buszugriff, Arbitration, Fehlererkennung
- **TLP-CAN/1** = Bedeutung von Identifiern und Payloads

---

## 2) Node-IDs und Rollen

- `node_id = 0x00`: reserviert für **Zentrale** und **Broadcast-Ziel**
- `node_id = 0x01` bis `0x1E`: gültige Node-IDs
- `node_id = 0x1F`: reserviert, nicht verwenden

Regeln:
- Uplink-Nachrichten (Node -> Zentrale) tragen immer die **Quell-Node-ID**
- Downlink-Nachrichten (Zentrale -> Node) tragen die **Ziel-Node-ID**
- `node_id = 0x00` in Downlink-Nachrichten bedeutet **Broadcast an alle Nodes**

---

## 3) CAN-Identifier-Schema

Der 11-Bit-Identifier ist fest aufgeteilt:

```text
Bit 10..9   Priorität
Bit  8..5   msg_type
Bit  4..0   node_id
```

Formel:

```text
can_id = (priority << 9) | (msg_type << 5) | node_id
```

### Prioritätsklassen

Niedrigere CAN-ID gewinnt die Arbitration. Deshalb gelten:

- `0b00` = **kritisch / ereignisgetrieben**
- `0b01` = **Befehle und Antworten**
- `0b10` = **zyklischer Status**
- `0b11` = **Bulk / Debug / niedrige Priorität**

### Feste Priorität je Nachrichtentyp

| Nachricht | priority | msg_type | Richtung | Zweck |
|---|---:|---:|---|---|
| `HEARTBEAT` | `0b10` | `0x1` | Node -> Zentrale | Lebenszeichen / Zustand |
| `SET_THRESHOLD` | `0b01` | `0x2` | Zentrale -> Node/Broadcast | Schwellwert setzen |
| `CMD_FLAP` | `0b01` | `0x3` | Zentrale -> Node/Broadcast | Klappe steuern (UP/DOWN/TOGGLE/AUTO/RESET) |
| `RESULT` | `0b01` | `0x4` | Node -> Zentrale | ACK/NACK für Befehle |
| `SENSOR_SUMMARY` | `0b10` | `0x5` | Node -> Zentrale | Sensor-Rohwerte für Threshold-Optimierung |

Rationale:
- `HIT_EVENT` hat höchste Priorität, damit Treffer nicht von Debug- oder Statusdaten verdrängt werden.
- Konfigurations- und Steuerbefehle liegen über zyklischen Statusmeldungen.
- `SENSOR_WINDOW` ist absichtlich niedrig priorisiert, weil diese Daten nur für Debug/Kalibrierung gedacht sind.

---

## 4) Gemeinsame Kodierregeln

- **Endianess:** alle Mehrbyte-Felder sind **Little Endian**
- **Integer-Typen:** unsigned, wenn nicht anders angegeben
- **Zeitwerte:** Millisekunden (`ms`), außer wenn explizit `us` oder `s` angegeben ist
- **Sensorwerte:** rohe oder vorverarbeitete Amplitudenwerte in **ADC-LSB**
- **Flags:** bitcodiert, Bit 0 ist das niederwertigste Bit
- **Reservierte Bytes/Bits:** beim Senden auf `0` setzen, beim Empfangen ignorieren
- **Unbekannte Nachrichtentypen:** ignorieren
- **Nicht unterstützte Befehle:** mit `RESULT.status = 0x02` beantworten

### Zähler und Zeitbasis

- `req_id`, `summary_counter`, `event_counter`, `window_id`, `chunk_index` laufen modulo 256
- `event_time_ms16` ist ein lokaler 16-Bit-Millisekunden-Zähler und darf überlaufen
- Die Zentrale nutzt für globale Historien zusätzlich ihre eigene Empfangszeit

### Reservierte Werte

- `0xFF` bzw. `0xFFFF` bedeutet, wenn sinnvoll, **unbekannt / nicht verfügbar**
- Broadcast-Befehle werden **nicht** individuell bestätigt

---

## 5) Nachrichtentypen und Payloads

### 5.1 `HEARTBEAT`

CAN-ID: `priority=0b10`, `msg_type=0x1`, `node_id=<source>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `proto_major` | `u8` | Protokoll-Hauptversion |
| 1 | `proto_minor` | `u8` | Protokoll-Nebenversion |
| 2 | `fw_version` | `u8` | freie Firmware-Revisionsnummer des Nodes |
| 3 | `status_flags` | `u8` | siehe unten |
| 4..7 | `uptime_s` | `u32` | Sekunden seit Start |

`status_flags` (aktueller Firmware-Stand):
- Bit 0: Klappe oben
- Bit 1: AUTO-Modus aktiv
- Bit 2: Klappe unten (Relais aktiv)
- Bit 3..7: reserviert

Empfehlung: alle 500 bis 1000 ms senden.

Ack-Regel: **kein ACK**

---

### 5.2 `SENSOR_SUMMARY`

CAN-ID: `priority=0b10`, `msg_type=0x5`, `node_id=<source>`

Diese Nachricht übermittelt Sensor-Statistiken für Monitoring und Threshold-Optimierung.

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0..1 | `current_raw` | `u16` | aktueller Sensor-Rohwert (ADC oder vorverarbeitet) |
| 2..3 | `peak` | `u16` | Peak-Wert seit letztem Report |
| 4..5 | `noise_floor` | `u16` | geschätztes Grundrauschen oder RMS |
| 6 | `summary_counter` | `u8` | laufender Report-Zähler |
| 7 | `summary_flags` | `u8` | Bit 0 = Sensor aktiv / bereit |

**Zweck der Felder für Threshold-Optimierung:**
- `current_raw`: Echtzeit-Messwert zum Beobachten des Noise-Pegels
- `peak`: Maximum seit letztem Report – hilft zu erkennen, ob echte Events vorhanden sind
- `noise_floor`: Basis-Pegel für ruhige Zeiten – Threshold sollte deutlich über diesem Wert liegen
- Empfehlter Threshold: `noise_floor * 2` bis `noise_floor * 3`

Empfehlung: alle 500–1000 ms (oder schneller für interaktive Threshold-Kalibrierung in der UI).

Ack-Regel: **kein ACK**

---

### 5.3 `SET_THRESHOLD`

CAN-ID: `priority=0b01`, `msg_type=0x2`, `node_id=<target oder 0x00 für Broadcast>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `req_id` | `u8` | Befehls-ID für spätere Antwort |
| 1..2 | `threshold` | `u16` | neuer Auslöseschwellwert (Rohdaten-Einheiten) |
| 3 | `apply_flags` | `u8` | Bit 0 = persistent speichern |
| 4..7 | reserviert |  | auf `0` setzen |

Ack-Regel:
- **unicast:** `RESULT` erforderlich
- **broadcast:** kein ACK

Hinweis: Mit `SENSOR_SUMMARY` (msg_type 0x5) bekommst du `current_raw`, `peak` und `noise_floor` übermittelt. Empfohlene Threshold-Formel: `noise_floor * 2` bis `noise_floor * 3`.

---

### 5.4 `CMD_FLAP`

CAN-ID: `priority=0b01`, `msg_type=0x3`, `node_id=<target oder 0x00 für Broadcast>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `req_id` | `u8` | Befehls-ID |
| 1 | `action` | `u8` | `0x00=UP`, `0x01=DOWN`, `0x02=TOGGLE`, `0x03=AUTO`, `0x04=RESET` |
| 2..7 | reserviert |  | auf `0` setzen |

Ack-Regel:
- **unicast:** `RESULT` erforderlich
- **broadcast:** kein ACK

---

### 5.5 `RESULT`

CAN-ID: `priority=0b01`, `msg_type=0x4`, `node_id=<source>`

`RESULT` ist die gemeinsame Antwort auf bestätigungspflichtige Befehle.

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `request_msg_type` | `u8` | bestätigter Nachrichtentyp (`0x2` oder `0x3`) |
| 1 | `request_id` | `u8` | Echo der Befehls-ID |
| 2 | `status` | `u8` | siehe Tabelle unten |
| 3 | `data0` | `u8` | optionaler Zusatzwert |
| 4 | `data1` | `u8` | optionaler Zusatzwert |
| 5..7 | reserviert |  | aktuell ungenutzt |

`status`:
- `0x00` = OK / angewendet
- `0x01` = ungültiger Wert
- `0x02` = nicht unterstützt

Retry-Empfehlung für die Zentrale:
- Unicast-Befehl senden
- auf `RESULT` warten
- bei Timeout 1 bis 2 Wiederholungen mit gleicher `req_id`
- danach Fehler an Benutzeroberfläche melden

---

## 6) Versions- und Kompatibilitätsregeln

Die Protokollversion ist in `HEARTBEAT.proto_major` und `HEARTBEAT.proto_minor` kodiert.

- **Major-Version** erhöht sich bei inkompatiblen Änderungen
- **Minor-Version** erhöht sich bei rückwärtskompatiblen Erweiterungen

Kompatibilitätsregeln:
- gleiche `proto_major` ist Pflicht
- eine höhere `proto_minor` darf zusätzliche, bisher reservierte Bits/Felder nutzen
- Empfänger müssen unbekannte reservierte Bits ignorieren
- Sender dürfen reservierte Felder nur mit `0` senden, solange sie nicht offiziell definiert sind

Empfehlung für aktuelle Implementierung:
- dieses Dokument definiert **`proto_major = 1`, `proto_minor = 1`**
- `proto_minor = 1` = `SENSOR_SUMMARY` hinzugefügt für Threshold-Kalibrierung

---

## 7) Praktische Implementierungshinweise

- Der Node sendet `HEARTBEAT` alle 500–1000 ms für Zustandsüberwachung.
- Der Node sendet `SENSOR_SUMMARY` alle 500–1000 ms mit Rohwerten für interaktive Threshold-Optimierung.
- Die zentrale UI nutzt `SENSOR_SUMMARY` (`current_raw`, `peak`, `noise_floor`), um dem Benutzer beim Schwellwert-Setzen zu helfen.
- `CMD_FLAP` und `SET_THRESHOLD` sind Befehle mit `RESULT`-Quittierung.
- Broadcast eignet sich für Startkonfiguration, aber nicht für sichere Quittierung.
- Die Zentrale sollte pro Node den letzten `summary_counter` von `SENSOR_SUMMARY` mitführen, um Lücken zu erkennen.

---

## 8) Implementierter Nachrichtenumfang

Die aktuelle Firmware unterstützt:
- ✓ `HEARTBEAT` – Lebenszeichen / Zustand
- ✓ `SENSOR_SUMMARY` – Sensor-Rohwerte (current_raw, peak, noise_floor) für Threshold-Kalibrierung
- ✓ `SET_THRESHOLD` – Auslöseschwellwert setzen
- ✓ `CMD_FLAP` – Klappe steuern (UP/DOWN/TOGGLE/AUTO/RESET)
- ✓ `RESULT` – Bestätigung für Befehle

Zukünftige Erweiterungen (reserviert):
- `HIT_EVENT` (msg_type 0x6) – optionales Treffer-Event mit Peak + Timestamp
- `SENSOR_WINDOW_BEGIN` / `SENSOR_WINDOW_CHUNK` (msg_types 0x7, 0x8) – fragmentierte Sensor-Fenster für Debugging
