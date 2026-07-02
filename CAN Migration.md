## Plan: CAN Migration mit MCP2515+TJA1050 Modulen

Du planst die Bestellung von MCP2515 SPI-CAN-Modulen mit TJA1050. Das ist für dein Projekt passend und vereinfacht die Integration deutlich: Diese Module enthalten bereits CAN-Controller und Transceiver. Damit ist das separate AZDelivery-Transceiver-Board für den ersten Ausbauschritt nicht mehr notwendig.

**Schritte**
1. Hardware-Basis festlegen: Pro Teilnehmer ein MCP2515+TJA1050-Modul verwenden (ATmega-Nodes und NodeMCU-Zentrale). AZDelivery-Boards optional als Reserve halten, aber nicht parallel zum TJA1050 am gleichen Teilnehmer einsetzen.
2. Spannungs-/Pegelstrategie festlegen: Viele MCP2515+TJA1050-Module sind 5V-versorgt und haben 5V-Logik am SPI-Header. Für den NodeMCU (3.3V) muss vor Verdrahtung sichergestellt werden, dass das Modul 3.3V-logikkompatibel ist oder ein Pegelwandler für SCK/MOSI/CS/INT eingesetzt wird. MISO vom Modul zum NodeMCU darf 3.3V nicht überschreiten.
3. Verdrahtung ATmega328P-Node planen: CS->D10, MOSI->D11, MISO->D12, SCK->D13, INT->D2, plus VCC/GND und CANH/CANL. Relais/Sensor bleiben lokal wie bisher am Node.
4. Verdrahtung NodeMCU-Zentrale planen: CS->D8, MOSI->D7, MISO->D6, SCK->D5, INT->D2, plus VCC/GND und CANH/CANL. Zusätzlich Boot-Pin-Verhalten prüfen (D8/GPIO15 muss beim Boot korrekt low bleiben).
5. Bus-Topologie festlegen: Linienbus mit genau zwei 120-Ohm-Abschlüssen (nur an den physikalischen Enden), kurze Abzweige, gemeinsamer GND-Bezug zwischen Knoten, Start mit 125 kbit/s.
6. Protokollumfang für Milestone 1 festlegen: HEARTBEAT, SENSOR_SUMMARY, HIT_EVENT, SET_THRESHOLD, SET_PARAMS, CMD_FLAP, RESULT gemäß CAN_PROTOCOL.md. SENSOR_WINDOW bewusst aus Milestone 1 ausklammern.
7. Firmware-Plan Node erstellen (parallel pro Node umsetzbar): In TLP-ATmega328P MCP2515 initialisieren, Sensorpipeline aus Phase-A übernehmen, Trefferlogik und Uplink-Messages senden, Downlink-Befehle anwenden und RESULT bei Unicast zurückgeben.
8. Firmware-Plan Zentrale erstellen (abhängig von Schritt 6 und 7): In TLP-NodeMCU lokale A0/Relais-Logik entfernen, RX/TX-CAN-Dispatcher einführen, NodeState-Tabelle pflegen, API auf node-parameterisierte Befehle umstellen, Online-Status per Heartbeat-Timeout berechnen.
9. Frontend-Plan erstellen (abhängig von Schritt 8): app.js und index.html auf echte Multi-Node-Daten mit node_id, online/offline und node-spezifischen Aktionen anpassen.
10. Inbetriebnahmeplan festlegen: Erst 1 Node + Zentrale, dann 2-3 Nodes, dann Zielgröße. Bei jedem Schritt Busfehler, Heartbeat-Latenz und Befehl/RESULT-Roundtrip messen.

**Relevant files**
- f:/TLP-Klappen/TLP-ATmega328P/src/main.cpp — CAN-Node-Firmware und lokale Trefferlogik
- f:/TLP-Klappen/TLP-ATmega328P/platformio.ini — MCP2515-Bibliothek/Konfiguration
- f:/TLP-Klappen/TLP-NodeMCU/src/main.cpp — CAN-Zentrale, NodeState, API/SSE-Quellen
- f:/TLP-Klappen/TLP-NodeMCU/platformio.ini — MCP2515-Bibliothek/Konfiguration
- f:/TLP-Klappen/TLP-NodeMCU/data/app.js — Multi-Node-UI-Logik
- f:/TLP-Klappen/TLP-NodeMCU/data/index.html — Node-Darstellung und Bedienung
- f:/TLP-Klappen/CAN_PROTOCOL.md — Protokollreferenz

**Verification**
1. Vorverdrahtungs-Check: Modulspannung und SPI-Pegel zum NodeMCU validiert, bei Bedarf Pegelwandler eingeplant.
2. Basisfunktion: MCP2515-Init auf Node und Zentrale erfolgreich bei 125 kbit/s.
3. Uplink-Test: HEARTBEAT und SENSOR_SUMMARY kommen stabil in der Zentrale an.
4. Event-Test: HIT_EVENT wird bei Treffer zuverlässig empfangen.
5. Downlink-Test: SET_THRESHOLD/CMD_FLAP von Web-UI erreicht Ziel-Node und RESULT kommt zurück.
6. Langzeittest: 30-60 Minuten ohne Bus-Off, ohne dauerhafte Error-Passive-Phasen.

**Decisions**
- Enthalten: Umstieg auf MCP2515+TJA1050 pro Teilnehmer, TLP-CAN/1-Minimalumfang, schrittweise Inbetriebnahme.
- Ausgeschlossen in Milestone 1: SENSOR_WINDOW, automatisches Provisioning, Persistenz-Feinheiten.
- Technische Leitentscheidung: Für den ersten Ausbau keine getrennten AZDelivery-Transceiver verwenden, da durch TJA1050 bereits abgedeckt.

**Further Considerations**
1. Beschaffung: Bevorzugt MCP2515-Module wählen, die explizit 3.3V-Logik am SPI unterstützen; das spart Pegelwandler und reduziert Integrationsrisiko am ESP8266.
2. Lagerstrategie: Bereits vorhandene AZDelivery-Boards als Ersatzteile behalten, aber nicht zusätzlich in denselben Knoten einfügen.
