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

## 7. TLP-Local und Moduswechsel
1. Ohne PC starten: AP, WLAN-Dashboard, HTTP-API und SSE funktionieren unveraendert.
2. Nur USB-Port oeffnen, HELLO senden oder ungueltige Daten eingeben: AP bleibt an.
3. TLP-Local mit Node.js 22+ starten, in Edge und Chrome mit NodeMCU verbinden.
4. Nach bestaetigtem ENTER_LOCAL sind AP/HTTP/SSE aus; CAN und Heartbeats laufen weiter.
5. Alle Einzelaktionen, Schwellwertgrenzen 0/1023, Sammelaktionen und fuenf
	Reaktionsrunden pruefen. Alte Messungen duerfen nicht als neue Treffer gelten.
6. Offline-Node, fehlende ACKs, volle Pending-Queue und ungueltige Befehle zeigen
	Fehler, fuehren aber nicht zu Wiederholung oder ungewollter Klappenbewegung.
7. Trennen aktiviert AP und gibt COM frei; WLAN-Browser verbindet SSE erneut.
8. Browser schliessen/abstuerzen, Hintergrundtab >1 Minute, Sleep/Wake und USB-Abbruch
	pruefen: ohne gueltigen PING nach 10 s plus AP-Startzeit wieder WLAN, kein CAN-Neustart.
9. USB-Abbruch mit externer Versorgung testen. Bei USB-only stattdessen Neustart mit
	WLAN-Default nach Wiederanschluss pruefen.
10. Falsches Geraet, belegter COM-Port, inkompatible Protokollversion und Portabbruch
	 melden einen Fehler. Wiederverbinden ist explizit; alte Befehle bleiben verworfen.
11. Mindestens 20 WLAN/Local-Wechsel und 15 Minuten Last mit bis zu 32 Nodes pruefen:
	 keine Handlerduplikate, UART-Ueberlaeufe, Heap-Abwaertsspirale oder neuen CAN-Verluste.
12. Bei gleicher Last freeHeap, maxFreeBlock, maxLoopUs und CAN-Zaehler in beiden Modi
	 vergleichen. Flash-/Build-RAM sind keine Messung der Laufzeitersparnis.

## 8. Automatisierte Local-Pruefung
1. In TLP-Local `npm.cmd test`: Node-Transport-/Servertests und echter C++-Parser
	(Host-g++ oder CXX; andernfalls wird der Parsertest als uebersprungen gemeldet).
2. Optional nach Chromium-Installation `npm.cmd run test:browser`: simulierte USB-
	sowie WLAN-Ablaufe, zwoelf Nodes, Schwellwert, fuenf Reaktionsrunden und mobile Breite.
3. NodeMCU mit PlatformIO bauen, danach fuer den Geraetetest Firmware und LittleFS
	mit den vorhandenen Upload-Tasks installieren. Serial-Port vorher freigeben.
