# Parksystem M450

## Gruppenmitglieder

- Ramon Zielke
- Laurent Scherrer

## Kurze Beschreibung der Umsetzung

Ein Raspberry Pi Pico W (Arduino, Core [arduino-pico](https://github.com/earlephilhower/arduino-pico)) steuert beide Schranken über zwei SG90-Servos:

| Schranke | Farbe | Signal | VCC (rot) | GND (braun) |
|---|---|---|---|---|
| Einfahrt | grün | GP8 (Pin 11) | VBUS (Pin 40) | GND (Pin 8) |
| Ausfahrt | rot | GP9 (Pin 12) | VBUS (Pin 40) | GND (Pin 13) |

Der Pico öffnet ein eigenes WLAN **`Parksystem`** (Passwort `parkhaus123`). Unter **http://192.168.4.1** gibt es eine Weboberfläche mit:

- **Kamera-Simulation:** Kennzeichen eingeben, dann Einfahrt/Ausfahrt auslösen, plus Hilfe-/Klingelknopf
- **Kasse** und **Automat:** Betrag anzeigen, Zahlung bestätigen oder Ablehnung simulieren
- **Übersicht** der parkierten Fahrzeuge
- **Testhilfe:** Einfahrts- und Bezahlzeit zurücksetzen (für die Grenzwerttests im Testkonzept)

Alles funktioniert auch über den seriellen Monitor (115200 Baud, `help` zeigt die Befehle). Die Uhrzeit wird vom Browser übernommen.

Regeln (gemäss Testkonzept, Annahmen A1–A3):
- Kennzeichen: 2 Buchstaben + 1–6 Ziffern (z.B. `ZH 123456`)
- CHF 2.00 pro angefangene Stunde
- 15 Minuten Kulanzzeit nach der Bezahlung, danach wird die zusätzliche Zeit nachberechnet
- Die Schranke schliesst 5 Sekunden nach dem Öffnen automatisch

### Bauen und Flashen

```sh
arduino-cli config add board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core install rp2040:rp2040
arduino-cli compile -b rp2040:rp2040:rpipicow --output-dir Parksystem/build Parksystem
```

BOOTSEL gedrückt halten, den Pico einstecken und `Parksystem/build/Parksystem.ino.uf2` auf das Laufwerk `RPI-RP2` kopieren.

## Umgesetzte User Stories

| US | Titel | Umsetzung |
|---|---|---|
| US-01 | Kennzeichenerfassung bei Einfahrt | Kennzeichen und Zeitstempel werden gespeichert, Fehlversuche im seriellen Log protokolliert |
| US-02 | Automatisches Öffnen der Einfahrtsschranke | Die grüne Schranke öffnet sofort und schliesst nach 5 s |
| US-03 | Ablehnung bei ungültigem Kennzeichen | Die Schranke bleibt zu und es erscheint ein Hinweis. Der Hilfe-Knopf informiert das Personal (Log + LED) |
| US-04 | Einfahrtszeit speichern | Die Einfahrtszeit wird gespeichert, eine doppelte Einfahrt wird als Konflikt gemeldet |
| US-05 | Gebührenberechnung | Anzeige von Einfahrt, aktueller Zeit, Dauer, Tarif und Betrag |
| US-06 | Bezahlung an Kasse oder Automat | Beide Ansichten vorhanden, abgelehnte Zahlung lässt den Status auf „offen“ |
| US-07 | Ausfahrt nur nach Bezahlung | Die rote Schranke öffnet nur, wenn nichts mehr offen ist. Ein unbekanntes Fahrzeug wird abgelehnt |
| US-08 | Kulanzzeit 15 Minuten | Innerhalb von 15 Min. freie Ausfahrt, danach Nachzahlung |
| US-09 | Übersicht parkierter Fahrzeuge | Tab „Übersicht“ / Befehl `liste` |
| US-10 | Quittung | Wird nach jeder erfolgreichen Zahlung angezeigt |
