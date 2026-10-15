/*
  Parksystem M450 - Pico 1 (Kasse)
  Raspberry Pi Pico W, Arduino (Core: earlephilhower/arduino-pico)

  Aufgaben (Testkonzept Kap. 1.2):
    - Oeffnet das WLAN "Parksystem" (Passwort "parkhaus123"), IP 192.168.4.1
    - Weboberflaeche fuer Kasse, Automat, Uebersicht und Testhilfe
    - Speichert Fahrzeuge, Einfahrtszeit und Bezahlstatus (bleibt nach Neustart erhalten, A5)
    - Berechnet die Parkgebuehr
    - Beantwortet die Anfragen von Pico 2 (Schranken) bei Ein- und Ausfahrt

  Ein-Pico-Betrieb:
    Pico 1 kann das ganze System auch alleine betreiben. Dann haengen die Servos an Pico 1
    (gruene Schranke GP8, rote Schranke GP9) und die Kamera-Simulation ist der erste Tab der
    Weboberflaeche. Mit zwei Picos bleiben GP8/GP9 an Pico 1 einfach frei.

  Schnittstelle fuer Pico 2 (Antwort: erste Zeile "JA" oder "NEIN", danach die Meldung):
    /intern/einfahrt?kz=ZH123456   Fahrzeug registrieren
    /intern/ausfahrt?kz=ZH123456   Darf das Fahrzeug ausfahren? (bei JA wird es ausgetragen)
    /intern/ping                   Antwort: JA

  Regeln (Testkonzept, Annahmen A1-A3):
    - Kennzeichen: 2 Buchstaben + 1-6 Ziffern
    - CHF 2.00 pro angefangene Stunde
    - 15 Minuten Kulanzzeit nach der Bezahlung
*/

#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <Servo.h>
#include "webpage.h"

const char *AP_SSID = "Parksystem";
const char *AP_PASS = "parkhaus123";

const int TARIF_RAPPEN_PRO_STUNDE = 200;
const uint32_t KULANZ_SEK = 15 * 60;
const int MAX_FAHRZEUGE = 50;
const uint32_t SPEICHER_MAGIC = 0x50415231;  // "PAR1"
const int MAX_FEHLVERSUCHE = 3;

// ---------- Schranken (nur im Ein-Pico-Betrieb angeschlossen) ----------
const int PIN_SERVO_EINFAHRT = 8;   // gruene Schranke
const int PIN_SERVO_AUSFAHRT = 9;   // rote Schranke
const int WINKEL_ZU = 90;
const int WINKEL_OFFEN = 0;
const unsigned long SCHRANKE_OFFEN_MS = 5000;   // US-02: schliesst nach Durchfahrt automatisch
const unsigned long SERVO_SCHRITT_MS = 15;      // 90 Grad in ca. 1.4 s (innert 2 s offen)
const unsigned long SERVO_AUS_NACH_MS = 400;    // im Stillstand abschalten, kein Dauerstrom

struct Fahrzeug {
  bool belegt;
  char kennzeichen[12];
  uint32_t einfahrt;     // Epoch-Sekunden
  uint32_t bezahltUm;    // 0 = nicht bezahlt
  int32_t bezahltRappen;
};

struct Ergebnis {
  bool ok;
  String meldung;
};

struct Speicher {
  uint32_t magic;
  uint32_t letzteZeit;   // damit die Uhr nach einem Neustart ungefaehr stimmt
  Fahrzeug fahrzeuge[MAX_FAHRZEUGE];
};

struct Schranke {
  Servo servo;
  int pin;
  const char *name;
  int winkel;
  int ziel;
  unsigned long letzterSchritt;
  unsigned long geoeffnetUm;
};

Speicher daten;
WebServer server(80);
Schranke einfahrt = {Servo(), PIN_SERVO_EINFAHRT, "Einfahrt", WINKEL_ZU, WINKEL_ZU, 0, 0};
Schranke ausfahrt = {Servo(), PIN_SERVO_AUSFAHRT, "Ausfahrt", WINKEL_ZU, WINKEL_ZU, 0, 0};
int fehlversuche = 0;

// Uhr: wird vom Browser bzw. von Pico 2 mit jeder Anfrage synchronisiert
uint32_t epochBasis = 0;
unsigned long millisBasis = 0;
long zeitzoneSek = 2 * 3600;

// ======================================================================
// Hilfsfunktionen
// ======================================================================

uint32_t jetzt() {
  return epochBasis + (millis() - millisBasis) / 1000;
}

void setzeUhr(uint32_t epoch) {
  epochBasis = epoch;
  millisBasis = millis();
}

String zeitText(uint32_t t) {
  uint32_t lokal = t + zeitzoneSek;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (unsigned long)((lokal / 3600) % 24),
           (unsigned long)((lokal / 60) % 60), (unsigned long)(lokal % 60));
  return String(buf);
}

String dauerText(uint32_t sek) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu h %02lu min", (unsigned long)(sek / 3600), (unsigned long)((sek / 60) % 60));
  return String(buf);
}

String chf(int rappen) {
  char buf[16];
  snprintf(buf, sizeof(buf), "CHF %d.%02d", rappen / 100, rappen % 100);
  return String(buf);
}

void logMeldung(const String &s) {
  if (Serial && Serial.availableForWrite() > (int)s.length() + 14) {
    Serial.print("[" + zeitText(jetzt()) + "] ");
    Serial.println(s);
  }
}

// A1: 2 Buchstaben + 1-6 Ziffern -> "ZH 123456", sonst ""
String normalisiere(String eingabe) {
  eingabe.trim();
  eingabe.toUpperCase();
  String k = "";
  for (unsigned int i = 0; i < eingabe.length() && k.length() < 20; i++) {
    if (eingabe[i] != ' ') k += eingabe[i];
  }
  if (k.length() < 3 || k.length() > 8) return "";
  if (!isAlpha(k[0]) || !isAlpha(k[1])) return "";
  for (unsigned int i = 2; i < k.length(); i++) {
    if (!isDigit(k[i])) return "";
  }
  return k.substring(0, 2) + " " + k.substring(2);
}

Fahrzeug *finde(const String &kz) {
  if (kz == "") return nullptr;
  for (int i = 0; i < MAX_FAHRZEUGE; i++) {
    if (daten.fahrzeuge[i].belegt && kz == daten.fahrzeuge[i].kennzeichen) return &daten.fahrzeuge[i];
  }
  return nullptr;
}

uint32_t seit(uint32_t t) {
  uint32_t n = jetzt();
  return n > t ? n - t : 0;
}

// Minuten abgerundet, pro angefangene Stunde, mindestens 1 Stunde
int gebuehrFuer(uint32_t sekunden) {
  uint32_t stunden = (sekunden / 60 + 59) / 60;
  if (stunden == 0) stunden = 1;
  if (stunden > 10000) stunden = 10000;
  return stunden * TARIF_RAPPEN_PRO_STUNDE;
}

int offenerBetrag(const Fahrzeug &f) {
  if (f.bezahltUm == 0) return gebuehrFuer(seit(f.einfahrt));
  uint32_t seitZahlung = seit(f.bezahltUm);
  if (seitZahlung <= KULANZ_SEK) return 0;
  return gebuehrFuer(seitZahlung);  // Zeit nach der Zahlung wird nachberechnet (US-08)
}

// ======================================================================
// Schranken (Ein-Pico-Betrieb)
// ======================================================================

bool istOffen(const Schranke &s) {
  return s.ziel == WINKEL_OFFEN;
}

void oeffne(Schranke &s) {
  s.ziel = WINKEL_OFFEN;
  s.geoeffnetUm = millis();
  logMeldung(String("Schranke ") + s.name + " OFFEN");
}

void schliesse(Schranke &s) {
  s.ziel = WINKEL_ZU;
  logMeldung(String("Schranke ") + s.name + " ZU");
}

// Servo schrittweise Richtung Ziel bewegen, im Stillstand abschalten
void bewegeServo(Schranke &s) {
  if (s.winkel == s.ziel) {
    if (s.servo.attached() && millis() - s.letzterSchritt > SERVO_AUS_NACH_MS) s.servo.detach();
    return;
  }
  if (!s.servo.attached()) s.servo.attach(s.pin, 500, 2500, s.winkel);
  if (millis() - s.letzterSchritt < SERVO_SCHRITT_MS) return;
  s.letzterSchritt = millis();
  s.winkel += s.ziel > s.winkel ? 1 : -1;
  s.servo.write(s.winkel);
}

void pruefeAutoSchliessen(Schranke &s) {
  if (istOffen(s) && millis() - s.geoeffnetUm >= SCHRANKE_OFFEN_MS) schliesse(s);
}

// ======================================================================
// Speicherung (A5: Bezahlstatus bleibt nach Neustart erhalten)
// ======================================================================

void speichern() {
  daten.letzteZeit = jetzt();
  EEPROM.put(0, daten);
  EEPROM.commit();
}

void laden() {
  EEPROM.get(0, daten);
  if (daten.magic != SPEICHER_MAGIC) {
    memset(&daten, 0, sizeof(daten));
    daten.magic = SPEICHER_MAGIC;
  }
  setzeUhr(daten.letzteZeit);
}

// ======================================================================
// Geschaeftslogik
// ======================================================================

// US-01, US-04 (aufgerufen von Pico 2)
Ergebnis einfahrtRegistrieren(const String &eingabe) {
  String kz = normalisiere(eingabe);
  if (kz == "") return {false, "Kennzeichen ungueltig."};
  if (finde(kz)) {
    logMeldung("KONFLIKT: " + kz + " ist bereits eingefahren");
    return {false, "Konflikt: " + kz + " ist bereits im Parkhaus. Schranke bleibt geschlossen."};
  }
  for (int i = 0; i < MAX_FAHRZEUGE; i++) {
    Fahrzeug &f = daten.fahrzeuge[i];
    if (f.belegt) continue;
    f.belegt = true;
    strncpy(f.kennzeichen, kz.c_str(), sizeof(f.kennzeichen) - 1);
    f.kennzeichen[sizeof(f.kennzeichen) - 1] = 0;
    f.einfahrt = jetzt();
    f.bezahltUm = 0;
    f.bezahltRappen = 0;
    speichern();
    logMeldung("Einfahrt registriert: " + kz);
    return {true, "Willkommen " + kz + "! Einfahrt um " + zeitText(f.einfahrt) + "."};
  }
  return {false, "Parkhaus voll. Schranke bleibt geschlossen."};
}

// US-07, US-08 (aufgerufen von Pico 2)
Ergebnis ausfahrtPruefen(const String &eingabe) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = finde(kz);
  if (!f) {
    logMeldung("Ausfahrt abgelehnt, unbekannt: " + eingabe);
    return {false, "Fahrzeug unbekannt, bitte Personal kontaktieren."};
  }
  int offen = offenerBetrag(*f);
  if (offen > 0) {
    logMeldung("Ausfahrt abgelehnt, offen: " + kz + " " + chf(offen));
    String grund = f->bezahltUm == 0 ? "Parkgebuehr nicht bezahlt" : "Kulanzzeit von 15 Minuten ueberschritten";
    return {false, grund + ". Offener Betrag: " + chf(offen) + ". Bitte an Kasse oder Automat bezahlen."};
  }
  f->belegt = false;
  speichern();
  logMeldung("Ausfahrt: " + kz);
  return {true, "Gute Fahrt, " + kz + "!"};
}

// US-05
Ergebnis gebuehr(const String &eingabe) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = finde(kz);
  if (!f) return {false, "Fahrzeug unbekannt, bitte Personal kontaktieren."};
  String m = "Kennzeichen: " + kz +
             "\nEinfahrt: " + zeitText(f->einfahrt) +
             "\nAktuelle Zeit: " + zeitText(jetzt()) +
             "\nAufenthaltsdauer: " + dauerText(seit(f->einfahrt)) +
             "\nTarif: CHF 2.00 pro angefangene Stunde";
  if (f->bezahltUm != 0) m += "\nBereits bezahlt: " + chf(f->bezahltRappen) + " um " + zeitText(f->bezahltUm);
  m += "\nOffener Betrag: " + chf(offenerBetrag(*f));
  return {true, m};
}

// US-06, US-10
Ergebnis bezahlen(const String &eingabe, bool erfolgreich, const String &ort) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = finde(kz);
  if (!f) return {false, "Fahrzeug unbekannt, bitte Personal kontaktieren."};
  int betrag = offenerBetrag(*f);
  if (betrag == 0) return {true, "Nichts offen - " + kz + " ist bezahlt."};
  if (!erfolgreich) {
    logMeldung("Zahlung abgelehnt (" + ort + "): " + kz);
    return {false, "Zahlung fehlgeschlagen (Zahlungsmittel abgelehnt). Status bleibt 'offen'. Bitte erneut versuchen."};
  }
  f->bezahltUm = jetzt();
  f->bezahltRappen += betrag;
  speichern();
  logMeldung("Zahlung OK (" + ort + "): " + kz + " " + chf(betrag));
  return {true, "--- QUITTUNG ---\nKennzeichen: " + kz +
                "\nEinfahrt: " + zeitText(f->einfahrt) +
                "\nAufenthaltsdauer: " + dauerText(seit(f->einfahrt)) +
                "\nBezahlt: " + chf(betrag) +
                "\nZeit: " + zeitText(jetzt()) +
                "\nAusfahrt innert 15 Minuten moeglich."};
}

// US-09
Ergebnis uebersicht() {
  String s = "";
  int n = 0;
  for (int i = 0; i < MAX_FAHRZEUGE; i++) {
    const Fahrzeug &f = daten.fahrzeuge[i];
    if (!f.belegt) continue;
    n++;
    s += String(f.kennzeichen) + "  Einfahrt " + zeitText(f.einfahrt) + "  " + (f.bezahltUm ? "bezahlt" : "offen") + "\n";
  }
  return {true, String(n) + " Fahrzeug(e) im Parkhaus\n" + s};
}

// Testhilfe (A3)
Ergebnis testhilfe(const String &eingabe, const String &was, long minuten) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = finde(kz);
  if (!f) return {false, "Fahrzeug unbekannt."};
  if (minuten < 0) minuten = 0;
  if (minuten > 100000) minuten = 100000;
  uint32_t t = jetzt() - minuten * 60;
  if (was == "einfahrt") {
    f->einfahrt = t;
  } else {
    if (f->bezahltUm == 0) return {false, kz + " hat noch nicht bezahlt."};
    f->bezahltUm = t;
  }
  speichern();
  logMeldung("Testhilfe: " + kz + " " + was + " auf vor " + minuten + " Min");
  return {true, kz + ": " + was + " auf " + zeitText(t) + " gesetzt (vor " + minuten + " Min)."};
}

// Kamera-Simulation im Ein-Pico-Betrieb (US-01 bis US-04)
Ergebnis einfahren(const String &eingabe) {
  if (normalisiere(eingabe) == "") {
    fehlversuche++;
    logMeldung("Einfahrt abgelehnt, Kennzeichen ungueltig/nicht lesbar: '" + eingabe + "' (Fehlversuch " + fehlversuche + ")");
    String m = "Kennzeichen nicht erkannt oder ungueltig. Schranke bleibt geschlossen.";
    if (fehlversuche >= MAX_FEHLVERSUCHE) m += " Bitte Hilfe-Knopf druecken.";
    return {false, m};
  }
  Ergebnis e = einfahrtRegistrieren(eingabe);
  if (e.ok) {
    fehlversuche = 0;
    oeffne(einfahrt);
    e.meldung += " Schranke offen.";
  }
  return e;
}

// Kamera-Simulation im Ein-Pico-Betrieb (US-07, US-08)
Ergebnis ausfahren(const String &eingabe) {
  Ergebnis e = ausfahrtPruefen(eingabe);
  if (e.ok) {
    oeffne(ausfahrt);
    e.meldung += " Schranke offen.";
  }
  return e;
}

// US-03: Klingel/Hilfe
Ergebnis hilfeRufen() {
  logMeldung("!!! HILFE angefordert an der Einfahrt - Personal informiert !!!");
  return {true, "Personal wurde informiert. Bitte warten."};
}

Ergebnis zuruecksetzen() {
  for (int i = 0; i < MAX_FAHRZEUGE; i++) daten.fahrzeuge[i].belegt = false;
  speichern();
  logMeldung("Testdaten zurueckgesetzt");
  return {true, "Testdaten zurueckgesetzt."};
}

// ======================================================================
// Web
// ======================================================================

String jsonEscape(const String &s) {
  String r = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if (c == '\n') r += "\\n";
    else r += c;
  }
  return r;
}

void sendeJson(const Ergebnis &e) {
  server.send(200, "application/json",
              String("{\"ok\":") + (e.ok ? "true" : "false") + ",\"meldung\":\"" + jsonEscape(e.meldung) + "\"}");
}

void sendeIntern(const Ergebnis &e) {
  server.send(200, "text/plain", String(e.ok ? "JA" : "NEIN") + "\n" + e.meldung);
}

void syncUhr() {
  if (server.hasArg("t")) {
    uint32_t t = strtoul(server.arg("t").c_str(), nullptr, 10);
    if (t > 1600000000UL) setzeUhr(t);
  }
  if (server.hasArg("tz")) zeitzoneSek = server.arg("tz").toInt();
}

void setupWeb() {
  server.on("/", []() { server.send(200, "text/html", WEBPAGE); });

  // Kasse / Automat / Betreiber
  server.on("/api/gebuehr", []() { syncUhr(); sendeJson(gebuehr(server.arg("kz"))); });
  server.on("/api/zahlen", []() {
    syncUhr();
    sendeJson(bezahlen(server.arg("kz"), server.arg("ok") != "0", server.arg("ort")));
  });
  server.on("/api/uebersicht", []() { syncUhr(); sendeJson(uebersicht()); });
  server.on("/api/testhilfe", []() {
    syncUhr();
    sendeJson(testhilfe(server.arg("kz"), server.arg("was"), server.arg("min").toInt()));
  });
  server.on("/api/reset", []() { sendeJson(zuruecksetzen()); });

  // Kamera-Simulation und Schranken (Ein-Pico-Betrieb)
  server.on("/api/einfahrt", []() { syncUhr(); sendeJson(einfahren(server.arg("kz"))); });
  server.on("/api/ausfahrt", []() { syncUhr(); sendeJson(ausfahren(server.arg("kz"))); });
  server.on("/api/hilfe", []() { sendeJson(hilfeRufen()); });
  server.on("/api/status", []() {
    server.send(200, "application/json",
                String("{\"einfahrt\":") + (istOffen(einfahrt) ? "true" : "false") +
                ",\"ausfahrt\":" + (istOffen(ausfahrt) ? "true" : "false") + "}");
  });

  // Schnittstelle fuer Pico 2
  server.on("/intern/ping", []() { syncUhr(); sendeIntern({true, "Kasse bereit"}); });
  server.on("/intern/einfahrt", []() { syncUhr(); sendeIntern(einfahrtRegistrieren(server.arg("kz"))); });
  server.on("/intern/ausfahrt", []() { syncUhr(); sendeIntern(ausfahrtPruefen(server.arg("kz"))); });

  server.onNotFound([]() { server.send(404, "text/plain", "Nicht gefunden"); });
  server.begin();
}

// ======================================================================
// Setup / Loop
// ======================================================================

void setup() {
  Serial.begin(115200);
  EEPROM.begin(sizeof(Speicher));
  laden();

  // Servos beim Start nicht bewegen: zusammen mit dem WLAN-Start wuerde das die
  // USB-Versorgung einbrechen lassen. Sie werden erst beim ersten Oeffnen angesteuert.

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  setupWeb();

  rp2040.wdt_begin(8000);
  logMeldung("Pico 1 (Kasse) bereit: WLAN " + String(AP_SSID) + ", http://" + WiFi.softAPIP().toString());
}

void loop() {
  rp2040.wdt_reset();
  server.handleClient();
  bewegeServo(einfahrt);
  bewegeServo(ausfahrt);
  pruefeAutoSchliessen(einfahrt);
  pruefeAutoSchliessen(ausfahrt);
}
