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
| `HIT_EVENT` | `0b00` | `0x3` | Node -> Zentrale | Treffer sofort melden |
| `RESULT` | `0b01` | `0x9` | Node -> Zentrale | ACK/NACK für Befehle |
| `SET_THRESHOLD` | `0b01` | `0x6` | Zentrale -> Node/Broadcast | Schwellwert setzen |
| `SET_PARAMS` | `0b01` | `0x7` | Zentrale -> Node/Broadcast | Laufzeitparameter setzen |
| `CMD_FLAP` | `0b01` | `0x8` | Zentrale -> Node/Broadcast | Klappe steuern |
| `HEARTBEAT` | `0b10` | `0x1` | Node -> Zentrale | Lebenszeichen / Zustand |
| `SENSOR_SUMMARY` | `0b10` | `0x2` | Node -> Zentrale | zyklische Sensorwerte |
| `SENSOR_WINDOW_BEGIN` | `0b11` | `0x4` | Node -> Zentrale | Start eines Fenster-Transfers |
| `SENSOR_WINDOW_CHUNK` | `0b11` | `0x5` | Node -> Zentrale | Fragment eines Fenster-Transfers |

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

`status_flags`:
- Bit 0: Klappe oben / bereit
- Bit 1: Klappe unten / ausgelöst
- Bit 2: Relais gerade aktiv
- Bit 3: Sensorfehler erkannt
- Bit 4: Konfiguration seit Boot geändert
- Bit 5..7: reserviert

Empfehlung: alle 500 bis 1000 ms senden.

Ack-Regel: **kein ACK**

---

### 5.2 `SENSOR_SUMMARY`

CAN-ID: `priority=0b10`, `msg_type=0x2`, `node_id=<source>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0..1 | `peak` | `u16` | Peak-Wert seit letztem Report |
| 2..3 | `rms` | `u16` | RMS- oder Energie-Maß |
| 4..5 | `noise_floor` | `u16` | geschätztes Grundrauschen |
| 6 | `summary_counter` | `u8` | laufender Report-Zähler |
| 7 | `summary_flags` | `u8` | reserviert für spätere Erweiterungen |

Empfehlung: alle 20 bis 50 ms oder langsamer, falls Buslast reduziert werden soll.

Ack-Regel: **kein ACK**

---

### 5.3 `HIT_EVENT`

CAN-ID: `priority=0b00`, `msg_type=0x3`, `node_id=<source>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0..1 | `peak` | `u16` | Peak des Trefferereignisses |
| 2..3 | `threshold` | `u16` | zum Auslösezeitpunkt aktiver Schwellwert |
| 4..5 | `event_time_ms16` | `u16` | lokaler Millisekunden-Zähler, modulo 65536 |
| 6 | `event_counter` | `u8` | Ereigniszähler des Nodes |
| 7 | `event_flags` | `u8` | siehe unten |

`event_flags`:
- Bit 0: Relais ausgelöst
- Bit 1: `SENSOR_WINDOW` folgt
- Bit 2: Event während Refractory-Phase erkannt, aber unterdrückt
- Bit 3..7: reserviert

Ack-Regel: **kein ACK**

Begründung: Treffer sollen mit minimaler Latenz auf den Bus; der CAN-Layer bestätigt den Frame bereits auf Bit-Ebene.

---

### 5.4 `SENSOR_WINDOW_BEGIN`

CAN-ID: `priority=0b11`, `msg_type=0x4`, `node_id=<source>`

Diese Nachricht kündigt einen fragmentierten Fenster-Transfer an.

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `window_id` | `u8` | Transfer-ID, eindeutig pro Node modulo 256 |
| 1 | `sample_format` | `u8` | `0x01 = s8`, andere Werte reserviert |
| 2 | `sample_count` | `u8` | Anzahl Samples im Fenster |
| 3 | `chunk_count` | `u8` | Anzahl folgender `SENSOR_WINDOW_CHUNK` Frames |
| 4..5 | `sample_period_us` | `u16` | Zeit zwischen Samples |
| 6 | `trigger_index` | `u8` | Sample-Index des Triggers im Fenster |
| 7 | `related_event_counter` | `u8` | zugehöriger `HIT_EVENT.event_counter`, sonst `0xFF` |

Ack-Regel: **kein ACK**

---

### 5.5 `SENSOR_WINDOW_CHUNK`

CAN-ID: `priority=0b11`, `msg_type=0x5`, `node_id=<source>`

Für `sample_format = 0x01` enthält jedes Fragment bis zu 5 Samples als `s8`.

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `window_id` | `u8` | muss zu `SENSOR_WINDOW_BEGIN` passen |
| 1 | `chunk_index` | `u8` | startet bei `0` |
| 2 | `data_len` | `u8` | Anzahl gültiger Sample-Bytes in diesem Frame (`0..5`) |
| 3..7 | `data` | Byte[5] | Sample-Bytes, ungenutzte Bytes = `0` |

Chunking-Regeln:
- `chunk_count = ceil(sample_count / 5)`
- alle Chunks eines Fensters werden direkt nach `SENSOR_WINDOW_BEGIN` gesendet
- bei Buskonflikten dürfen Chunks verzögert werden; `HIT_EVENT`, Befehle und Status dürfen sie jederzeit verdrängen
- fehlt ein Chunk, verwirft die Zentrale das gesamte Fenster nach Timeout

Ack-Regel: **kein ACK**

---

### 5.6 `SET_THRESHOLD`

CAN-ID: `priority=0b01`, `msg_type=0x6`, `node_id=<target oder 0x00 für Broadcast>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `req_id` | `u8` | Befehls-ID für spätere Antwort |
| 1..2 | `threshold` | `u16` | neuer Auslöseschwellwert |
| 3 | `apply_flags` | `u8` | Bit 0 = persistent speichern |
| 4..7 | reserviert |  | auf `0` setzen |

Ack-Regel:
- **unicast:** `RESULT` erforderlich
- **broadcast:** kein ACK

---

### 5.7 `SET_PARAMS`

CAN-ID: `priority=0b01`, `msg_type=0x7`, `node_id=<target oder 0x00 für Broadcast>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `req_id` | `u8` | Befehls-ID |
| 1..2 | `refractory_ms` | `u16` | Sperrzeit nach Treffer |
| 3..4 | `relay_pulse_ms` | `u16` | Relais-Pulsdauer |
| 5..6 | `report_interval_ms` | `u16` | Sendeintervall für `SENSOR_SUMMARY` |
| 7 | `apply_flags` | `u8` | Bit 0 = persistent speichern |

Ack-Regel:
- **unicast:** `RESULT` erforderlich
- **broadcast:** kein ACK

---

### 5.8 `CMD_FLAP`

CAN-ID: `priority=0b01`, `msg_type=0x8`, `node_id=<target oder 0x00 für Broadcast>`

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `req_id` | `u8` | Befehls-ID |
| 1 | `action` | `u8` | `0x00=NOOP`, `0x01=UP`, `0x02=DOWN`, `0x03=TRIGGER` |
| 2..3 | `override_pulse_ms` | `u16` | `0` = Standardwert aus Parametern |
| 4..7 | reserviert |  | auf `0` setzen |

Ack-Regel:
- **unicast:** `RESULT` erforderlich
- **broadcast:** kein ACK

---

### 5.9 `RESULT`

CAN-ID: `priority=0b01`, `msg_type=0x9`, `node_id=<source>`

`RESULT` ist die gemeinsame Antwort auf bestätigungspflichtige Befehle.

| Byte | Feld | Typ | Beschreibung |
|---:|---|---|---|
| 0 | `req_id` | `u8` | Echo der Befehls-ID |
| 1 | `msg_type_ack` | `u8` | bestätigter Nachrichtentyp (`0x6`, `0x7`, `0x8`) |
| 2 | `status` | `u8` | siehe Tabelle unten |
| 3 | `result_flags` | `u8` | reserviert |
| 4..7 | `detail` | `u32` | optionaler Zusatzwert, sonst `0` |

`status`:
- `0x00` = OK / angewendet
- `0x01` = ungültiger Wert
- `0x02` = nicht unterstützt
- `0x03` = Node gerade beschäftigt
- `0x04` = interner Fehler

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

Empfehlung für erste Implementierung:
- dieses Dokument definiert **`proto_major = 1`, `proto_minor = 0`**

---

## 7) Praktische Implementierungshinweise

- Nur `HIT_EVENT` sollte wirklich latenzkritisch behandelt werden.
- `SENSOR_WINDOW` nur bei Bedarf senden, z. B. für Kalibrierung oder Debug.
- Broadcast eignet sich für Startkonfiguration, aber nicht für sichere Quittierung.
- Die Zentrale sollte pro Node den letzten `summary_counter`, `event_counter` und `window_id` mitführen, um Lücken zu erkennen.
- Ein Node darf empfangene Broadcast-Befehle lokal filtern, muss sie aber identisch zu Unicast anwenden.

---

## 8) Minimaler Soll-Umfang für künftige Implementierungen

Ein erster Node-Firmwarestand sollte mindestens unterstützen:
- `HEARTBEAT`
- `SENSOR_SUMMARY`
- `HIT_EVENT`
- `SET_THRESHOLD`
- `SET_PARAMS`
- `CMD_FLAP`
- `RESULT`

`SENSOR_WINDOW_BEGIN` und `SENSOR_WINDOW_CHUNK` bleiben optional, aber wenn implementiert, dann genau nach dieser Spezifikation.
