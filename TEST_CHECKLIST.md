# TLP-Klappen Regression Checkliste

Kurzcheck nach Firmware-/UI-Aenderungen.

## 1. Vorbereitung
1. NodeMCU und ATmega mit aktueller Firmware flashen.
2. NodeMCU LittleFS hochladen.
3. Pruefen, dass beide am gleichen CAN-Bus mit gemeinsamer GND haengen.

## 2. Basis-Health
1. NodeMCU `/api/status` liefert `canNormal=1`.
2. `onlineNodes >= 1` und Node `0x11` ist online.
3. `canResultCount` steigt nach Befehlen.

## 3. Command-Matrix (Soll)
1. `UP`: ACK, Klappe oben, AUTO aus.
2. `DOWN`: ACK, Klappe unten, AUTO aus.
3. `TOGGLE`: ACK, Zustand invertiert, AUTO aus.
4. `AUTO`: ACK, AUTO an, Klappe oben, Sensor-Hits duerfen wieder triggern.
5. `RESET`: ACK, eventCounter=0, envelope=0, AUTO an, Klappe oben.

## 4. API-Pruefung
1. `POST /api/flap?...` liefert `202` mit `requestId`.
2. `GET /api/command-result?id=...` geht von `pending` auf `ack`/`nack`.
3. Bei `ack`: UI zeigt Erfolg, bei `nack`/`send_failed`/`expired`: UI zeigt Fehler.

## 5. UI-Pruefung
1. Node-Karten werden node_id-basiert gerendert.
2. Online/Offline-Status je Karte stimmt mit Heartbeat ueberein.
3. Mode-Anzeige nutzt Heartbeat-Flag Bit 1 (AUTO).

## 6. Dauerlauf
1. 15 Minuten laufen lassen, keine Reboots/Watchdog-Resets.
2. Danach optional 30 und 60 Minuten wiederholen.
3. Auffaelligkeiten notieren: TX-Fehler, verpasste ACKs, Offline-Flattern.
