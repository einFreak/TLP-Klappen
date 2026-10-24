# TLP-Local

Die vorhandene TLP-Oberflaeche laeuft auf dem PC. Edge/Chrome kommuniziert direkt
ueber Web Serial mit dem USB-Port des NodeMCU. Der lokale Node.js-Server liefert
nur Dateien aus und greift nicht auf den COM-Port zu. Keine Serial-Bridge, kein
Internet und keine installierten npm-Pakete fuer den Betrieb erforderlich.

## Start

Voraussetzungen: Node.js 22 oder neuer, Desktop Edge/Chrome, USB-Datenkabel,
passender USB-UART-Treiber sowie die aktuelle NodeMCU-Firmware **und LittleFS**.
Die ATmega-Firmware und das CAN-Protokoll bleiben unveraendert.

```powershell
cd F:\TLP-Klappen\TLP-Local
npm.cmd start
```

Die ausgegebene URL (normalerweise http://localhost:8080) in Edge/Chrome oeffnen.
Bei belegtem Port wird ein freier Port gewaehlt und ausgegeben. Nur Loopback wird
gebunden; die Website ist nicht im LAN freigegeben. Optional: `PORT` setzen.

1. Serial-Monitor schliessen; er darf den Port nicht gleichzeitig belegen.
2. **USB verbinden** anklicken und den NodeMCU auswaehlen, nicht einen ATmega.
3. Der Browser prueft Geraet und Protokoll und fordert den lokalen Modus an.
4. Nach dessen Bestaetigung und vollstaendigem Anfangsstatus sind die Aktionen
   verfuegbar. WLAN und Webserver des NodeMCU sind jetzt abgeschaltet.
5. **Trennen / WLAN aktivieren** beendet den lokalen Modus und gibt den Port frei.
   Vor Uploads oder erneutem Serial-Monitor-Zugriff immer trennen.

Die gemeinsame UI bleibt unter `../TLP-NodeMCU/data`. Nur `/transport.js` wird
durch den lokalen Serial-Adapter ersetzt; es gibt keine zweite UI-Kopie.

## Verhalten bei Fehlern

- Jeder Neustart beginnt mit WLAN. Portoeffnung oder HELLO allein schalten nicht um.
- Ein gueltiger PING wird alle 2 Sekunden gesendet. Nach 10 Sekunden ohne PING
  startet die Firmware WLAN wieder, ohne CAN neu zu initialisieren oder eine
  Klappe zu bewegen. Die AP-Startdauer kommt zum Timeout hinzu.
- Bei fehlgeschlagenem AP-Start versucht die Firmware es alle 2 Sekunden erneut.
- Browser schliessen, Absturz, USB-Abbruch, PC-Standby oder stark gedrosselte
  Hintergrund-Tabs koennen den Rueckfall ausloesen. Danach bewusst neu verbinden;
  keine automatische Umschaltung und keine Wiederholung physischer Befehle.
- Bei reiner USB-Stromversorgung ist Kabelziehen ein Stromausfall. Erst nach
  Wiedereinschalten kann wieder WLAN starten. Fuer einen Timeout-Test extern speisen.
- USB-Oeffnen kann je nach Board einen Reset ausloesen. Der Adapter wartet beim
  Handshake begrenzt auf Boot-Bereitschaft; Boot-Logs werden nicht als Daten gewertet.
- Web Serial erfordert eine berechtigte Website (`localhost`) und einen Klick fuer
  die Portauswahl. Firefox und mobile Browser sind nicht Teil dieser Variante.
- Layout, bekannte Nodes und Bestzeiten bleiben im Browser gespeichert. WLAN und
  localhost (auch verschiedene Ports) haben getrennten localStorage.

## Serielles Protokoll TLP1

115200 Baud, 8N1, keine Hardware-Flusssteuerung. ASCII-Anfragen mit LF oder CRLF:

```text
TLP1 <id> <operation> <session> [argumente]
```

`id` ist pro Sitzung streng steigend, 1..2147483647; `session` ist eine vom
Controller gelieferte positive 32-Bit-Zahl. HELLO verwendet Session 0. Eine alte
oder doppelte Anfrage wird nicht erneut ausgefuehrt. Anfragen einschliesslich LF
maximal 128 Byte; nach Ueberlauf bis zum naechsten LF verwerfen. Ein unvollstaendiger
Empfang wird nach einer Sekunde verworfen. Werte muessen vollstaendige Dezimalzahlen
sein; keine Vorzeichen, Hexwerte, Brueche oder angehaengten Zeichen.

| Operation | Argumente | Bedeutung |
|---|---|---|
| HELLO | keine; Session 0 | Geraet, Protokollversion 1, Session, Lease; WLAN bleibt an |
| ENTER_LOCAL | keine | Nur nach HELLO; WLAN/HTTP/SSE aus, Snapshot beginnt |
| PING | keine | Nur gueltige lokale Sitzung verlaengert die Lease |
| EXIT_LOCAL | keine | WLAN wieder starten |
| STATUS | keine | Vollstaendigen Snapshot anfordern; laufende Anfragen zusammenfassen |
| FLAP | Node 0..31, up/down/toggle/auto/reset | Bestehenden CAN-Befehl senden |
| THRESHOLD | Node 0..31, Wert 0..1023 | Bestehenden Schwellwertbefehl senden |
| RESULT | CAN-requestId 1..255 | pending/ack/nack/send_failed/expired abfragen |

Antworten sind markierte JSON-Zeilen, maximal 512 Byte:

```text
TLP1 {"id":1,"session":12345,"type":"hello","data":{"device":"TLP-NodeMCU","version":1,"mode":"wifi","leaseMs":10000}}
TLP1 {"id":2,"session":12345,"type":"mode","data":{"mode":"local"}}
```

Antworttypen: `hello`, `mode`, `pong`, `ok`, `accepted`, `result`, `error`.
`id:0` kennzeichnet Statusereignisse: `snapshot_begin`, `central`, je eine `node`
pro bekannter Node, dann `snapshot_end`. Beide Snapshot-Grenzen enthalten dieselbe
`generation`. Anschliessend werden einzelne Node-Aenderungen und alle zwei Sekunden
Zentralendaten gesendet. `mode` steht in Zentralendaten auf `local`.

Der Browser gibt Aktionen erst nach einem vollstaendigen Snapshot frei und haelt
einen Status-Cache fuer UI und Spiele. Node-Updates werden zusammengefasst, wichtige
Zustands-/Timing-Aenderungen vor Sensortelemetrie bevorzugt und hoechstens eine
Node pro 100 ms aktualisiert. Bei 32 aktiven Nodes ist daher keine 100-ms-Aktualisierung
aller Sensoren garantiert. Initialsnapshots werden stueckweise uebertragen.

Serial-Anfrage-ID und CAN-requestId sind getrennte Nummernraeume. `accepted` bedeutet
nur angenommen; Erfolg ist erst `result.state == "ack"`. Lokale Steueraktionen
werden bis zur Bestaetigung serialisiert, fertige Serial-Ergebnisse geben ihren
Pending-Slot frei. Maximal acht CAN-Befehle koennen gleichzeitig vorgemerkt sein.
Verlorene Ergebnisse fuehren zu einem Fehler, nicht zu automatischem Wiederholen.

Die Rohwerte `lastUpTimeMs` kommen weiterhin vom ATmega. Der zusaetzliche
`lastUpEventSequence` zaehlt die am NodeMCU empfangenen Timing-Ereignisse, auch wenn
RESET den ATmega-Trefferzaehler zuruecksetzt. Die Sequenz beginnt beim NodeMCU-Neustart
wieder bei null. Debug-Ausgaben sind waehrend einer Protokollsitzung unterdrueckt.

## Pruefen und Messen

```powershell
npm.cmd test
npm.cmd exec --yes --package=@playwright/test -- playwright install chromium
npm.cmd run test:browser
```

`npm test` nutzt Node-Bordmittel, keine Testabhaengigkeiten. Der echte C++-Parser
wird mit `g++` oder dem Compiler in `CXX` gebaut und ausgefuehrt; ohne Host-Compiler
wird nur dieser Test als uebersprungen gemeldet. Der optionale Browsercheck laedt
Playwright in den npm-Cache, nicht als Laufzeitabhaengigkeit dieses Projekts. Er
prueft eine simulierte Serial-Gegenstelle, WLAN-API, Sammelaktionen, Schwellwerte,
fuenf Reaktionsrunden, Trennen sowie Desktop/Mobilbreite. Screenshots liegen im
Temp-Verzeichnis als `tlp-local-desktop.png` und `tlp-local-mobile.png`.

Firmware bauen (kein Upload):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir ..\TLP-NodeMCU --environment nodemcuv2
```

Am realen Geraet noch die Matrix in `../TEST_CHECKLIST.md` abarbeiten. Die
simulierten Tests belegen weder USB-Treiber-/DTR-Verhalten noch reale WLAN-Rueckkehr,
CAN-Latenzen oder Ressourcenersparnis.

`/api/status` (WLAN) und `TLPTransport.getStatus()` (lokale Browserkonsole) enthalten
`freeHeap`, `maxFreeBlock` und `maxLoopUs`. Letzteres ist die laengste Hauptschleife
seit Start/Moduswechsel, einschliesslich `yield()`. Der Umschaltvorgang selbst kann
die erste Probe beeinflussen. Bei gleicher Node-Zahl und Last WLAN mit/ohne UI
gegen lokal vergleichen und CAN-Zaehler auf Auffaelligkeiten pruefen.
WLAN-Code, globale Serverobjekte und LittleFS bleiben im Flash bzw. teilweise RAM;
eine Laufzeiteinsparung ist zu messen und kein versprochener fester Wert.