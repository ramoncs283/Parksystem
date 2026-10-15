# Parksystem M450

Parkhaus mit Ein- und Ausfahrtsschranke, Gebührenberechnung und Bezahlung. Umgesetzt mit zwei Raspberry Pi Pico W in Arduino (Modul M450, Aufgabe A01-03).

## Gruppenmitglieder

- Ramon Zielke
- Laurent Scherrer

## Kurze Beschreibung der Umsetzung

Die Umsetzung folgt dem Aufbau aus unserem Testkonzept: zwei Picos, die über WLAN miteinander kommunizieren.

| Komponente | Sketch | Aufgabe |
|---|---|---|
| **Pico 1 (Kasse)** | `Pico1_Kasse/` | Öffnet das WLAN `Parksystem`, zeigt die Weboberfläche für Kasse, Automat, Übersicht und Testhilfe, speichert Fahrzeuge und Bezahlstatus (bleibt nach einem Neustart erhalten), berechnet die Gebühr und beantwortet die Anfragen von Pico 2 |
| **Pico 2 (Schranken)** | `Pico2_Schranken/` | Verbindet sich mit dem WLAN von Pico 1, zeigt die Kamera-Simulation (Kennzeichen bei Ein- und Ausfahrt eingeben), prüft das Kennzeichenformat, fragt Pico 1 an und steuert beide Schranken |

```
Browser (Kasse/Automat)  ──>  Pico 1 Kasse      http://192.168.4.1
Browser (Kamera)         ──>  Pico 2 Schranken  http://192.168.4.2
                              Pico 2 ──(Einfahrt registrieren / Ausfahrt prüfen)──> Pico 1
```

WLAN: **`Parksystem`**, Passwort **`parkhaus123`**. Notebooks verbinden sich mit diesem WLAN.

### Verdrahtung (Pico 2)

| Schranke | Farbe | Signal (orange) | VCC (rot) | GND (braun) |
|---|---|---|---|---|
| Einfahrt | grün | GP8 (Pin 11) | VBUS (Pin 40) | GND (Pin 8) |
| Ausfahrt | rot | GP9 (Pin 12) | VBUS (Pin 40) | GND (Pin 13) |

Pico 1 braucht nur die USB-Stromversorgung.

Zwei SG90 über USB sind an der Grenze dessen, was der USB-Anschluss liefert. Die Servos fahren deshalb langsam und sind im Stillstand abgeschaltet. Für einen stabilen Betrieb lohnt sich trotzdem eine eigene 5-V-Versorgung für die Servos (deren GND mit dem GND des Picos verbinden).

### Regeln (gemäss Testkonzept, Annahmen A1–A5)

- Kennzeichen: 2 Buchstaben + 1–6 Ziffern, z.B. `ZH 123456` (Gross-/Kleinschreibung und Leerzeichen egal)
- CHF 2.00 pro angefangene Stunde, ohne Gratiszeit
- 15 Minuten Kulanzzeit nach der Bezahlung, danach wird die zusätzliche Zeit nachberechnet
- Die Schranke öffnet innert 2 Sekunden und schliesst 5 Sekunden nach dem Öffnen automatisch
- Kapazität: 50 Fahrzeuge
- Ist Pico 1 nicht erreichbar, bleiben die Schranken geschlossen und es erscheint eine Fehlermeldung
- Die Uhrzeit übernehmen die Picos vom Browser

### Bauen und Flashen

```sh
arduino-cli config add board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core install rp2040:rp2040
arduino-cli compile -b rp2040:rp2040:rpipicow --output-dir Pico1_Kasse/build Pico1_Kasse
arduino-cli compile -b rp2040:rp2040:rpipicow --output-dir Pico2_Schranken/build Pico2_Schranken
```

BOOTSEL gedrückt halten, den Pico einstecken und die `.uf2`-Datei aus dem `build`-Ordner auf das Laufwerk `RPI-RP2` kopieren. Pico 1 zuerst starten, damit das WLAN steht.

### Alternative: Mac-App mit einem Pico

Für Tests mit nur einem Pico gibt es eine zweite Variante:

- `mac-app/parksystem.py` enthält die ganze Logik und die Weboberfläche und läuft lokal auf dem Mac (Python 3, nur Standardbibliothek).
- `Schranken/Schranken.ino` läuft auf dem Pico und bewegt nur die Servos. Die Befehle kommen über USB (`E OPEN`, `E CLOSE`, `A OPEN`, `A CLOSE`, `PING`, `STATUS`).

```sh
python3 mac-app/parksystem.py      # öffnet http://localhost:8080
```

## Umgesetzte User Stories

| US | Titel | Umsetzung |
|---|---|---|
| US-01 | Kennzeichenerfassung bei Einfahrt | Pico 1 speichert Kennzeichen und Einfahrtszeit. Ungültige Kennzeichen werden abgelehnt und im Log protokolliert |
| US-02 | Automatisches Öffnen der Einfahrtsschranke | Die grüne Schranke öffnet innert 2 s und schliesst nach 5 s wieder |
| US-03 | Ablehnung bei ungültigem Kennzeichen | Die Schranke bleibt zu und es erscheint ein Hinweis. Nach 3 Fehlversuchen wird auf den Hilfe-Knopf hingewiesen, der das Personal informiert |
| US-04 | Einfahrtszeit speichern | Die Einfahrtszeit wird gespeichert, eine doppelte Einfahrt wird als Konflikt gemeldet |
| US-05 | Gebührenberechnung | Anzeige von Einfahrt, aktueller Zeit, Dauer, Tarif und Betrag |
| US-06 | Bezahlung an Kasse oder Automat | Beide Ansichten vorhanden. Eine abgelehnte Zahlung lässt den Status auf „offen“, ein erneuter Versuch ist möglich |
| US-07 | Ausfahrt nur nach Bezahlung | Pico 2 fragt Pico 1 an, die rote Schranke öffnet nur, wenn nichts mehr offen ist. Unbekannte Fahrzeuge werden abgelehnt |
| US-08 | Kulanzzeit 15 Minuten | Innerhalb von 15 Min. nach der Zahlung freie Ausfahrt, danach Nachzahlung |
| US-09 | Übersicht parkierter Fahrzeuge | Tab „Übersicht“ auf Pico 1 |
| US-10 | Quittung | Wird nach jeder erfolgreichen Zahlung angezeigt |

Zusätzlich gibt es auf Pico 1 eine **Testhilfe**, mit der sich die Einfahrts- und Bezahlzeit zurücksetzen lassen. Damit sind die Grenzwerttests aus dem Testkonzept (TC-06, TC-13, TC-14) ohne Wartezeit möglich.
