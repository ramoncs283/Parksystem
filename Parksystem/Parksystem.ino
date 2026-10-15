/*
  Parksystem M450 - Raspberry Pi Pico W (Arduino, Core: earlephilhower/arduino-pico)

  Ein Pico steuert beide Schranken:
    - Einfahrt  = Servo mit gruener Schranke an GP8 (Pin 11)
    - Ausfahrt  = Servo mit roter Schranke   an GP9 (Pin 12)

  Bedienung:
    - Weboberflaeche: WLAN "Parksystem" (Passwort "parkhaus123"), dann http://192.168.4.1
      Kamera-Simulation (Ein-/Ausfahrt), Kasse, Automat, Uebersicht, Testhilfe
    - Serieller Monitor (115200 Baud): "help" zeigt alle Befehle

  Tarif / Regeln (Testkonzept A01-10, Annahmen A1-A3):
    - Kennzeichen: 2 Buchstaben (Kanton) + 1-6 Ziffern, z.B. "ZH 123456"
    - CHF 2.00 pro angefangene Stunde, keine Gratiszeit
    - 15 Minuten Kulanzzeit zwischen Bezahlung und Ausfahrt
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Servo.h>
#include "webpage.h"

// ---------- Hardware ----------
const int PIN_SERVO_EINFAHRT = 8;   // gruene Schranke
const int PIN_SERVO_AUSFAHRT = 9;   // rote Schranke
const int WINKEL_ZU = 0;
const int WINKEL_OFFEN = 90;
const unsigned long SCHRANKE_OFFEN_MS = 5000;  // US-02: schliesst nach Durchfahrt automatisch
// Servo faehrt langsam (1 Grad pro Schritt), damit der Anlaufstrom die USB-Versorgung nicht einbrechen laesst
const unsigned long SERVO_SCHRITT_MS = 15;      // 90 Grad in ca. 1.4 s

// ---------- Netzwerk ----------
const char *AP_SSID = "Parksystem";
const char *AP_PASS = "parkhaus123";

// ---------- Tarif ----------
const int TARIF_RAPPEN_PRO_STUNDE = 200;
const uint32_t KULANZ_SEK = 15 * 60;
const int MAX_FEHLVERSUCHE = 3;
const int MAX_FAHRZEUGE = 50;

struct Fahrzeug {
  bool belegt;
  char kennzeichen[12];
  uint32_t einfahrt;     // Epoch-Sekunden
  uint32_t bezahltUm;    // 0 = nicht bezahlt
  int bezahltRappen;     // Summe aller Zahlungen
};

Fahrzeug fahrzeuge[MAX_FAHRZEUGE];

struct Ergebnis {
  bool ok;
  String meldung;
};

struct Schranke {
  Servo servo;
  int pin;
  bool offen;
  unsigned long geoeffnetUm;
  const char *name;
  int winkel;               // aktuelle Position
  int ziel;                 // Zielposition
  unsigned long letzterSchritt;
};

Schranke einfahrt = {Servo(), PIN_SERVO_EINFAHRT, false, 0, "Einfahrt", WINKEL_ZU, WINKEL_ZU, 0};
Schranke ausfahrt = {Servo(), PIN_SERVO_AUSFAHRT, false, 0, "Ausfahrt", WINKEL_ZU, WINKEL_ZU, 0};

WebServer server(80);
bool wlanAktiv = false;
int fehlversuche = 0;

// Uhr: wird vom Browser synchronisiert, sonst Laufzeit ab 0
uint32_t epochBasis = 0;
unsigned long millisBasis = 0;
bool uhrGesetzt = false;
long zeitzoneSek = 2 * 3600;  // wird vom Browser gesetzt

// ======================================================================
// Hilfsfunktionen
// ======================================================================

uint32_t jetzt() {
  return epochBasis + (millis() - millisBasis) / 1000;
}

void setzeUhr(uint32_t epoch) {
  epochBasis = epoch;
  millisBasis = millis();
  uhrGesetzt = true;
}

String zeitText(uint32_t t) {
  uint32_t lokal = t + zeitzoneSek;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           (unsigned long)((lokal / 3600) % 24), (unsigned long)((lokal / 60) % 60), (unsigned long)(lokal % 60));
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
  Serial.print("[");
  Serial.print(zeitText(jetzt()));
  Serial.print("] ");
  Serial.println(s);
}

// A1: 2 Buchstaben + 1-6 Ziffern. Gibt normalisiertes Kennzeichen "ZH 123456" zurueck oder "" bei ungueltig.
String normalisiere(String eingabe) {
  eingabe.trim();
  eingabe.toUpperCase();
  String ohneLeer = "";
  for (unsigned int i = 0; i < eingabe.length(); i++) {
    if (eingabe[i] != ' ') ohneLeer += eingabe[i];
  }
  if (ohneLeer.length() < 3 || ohneLeer.length() > 8) return "";
  if (!isAlpha(ohneLeer[0]) || !isAlpha(ohneLeer[1])) return "";
  for (unsigned int i = 2; i < ohneLeer.length(); i++) {
    if (!isDigit(ohneLeer[i])) return "";
  }
  return ohneLeer.substring(0, 2) + " " + ohneLeer.substring(2);
}

Fahrzeug *finde(const String &kz) {
  for (int i = 0; i < MAX_FAHRZEUGE; i++) {
    if (fahrzeuge[i].belegt && kz == fahrzeuge[i].kennzeichen) return &fahrzeuge[i];
  }
  return nullptr;
}

// Minuten abgerundet, dann pro angefangene Stunde (mind. 1 Stunde)
int gebuehrFuer(uint32_t sekunden) {
  uint32_t minuten = sekunden / 60;
  uint32_t stunden = (minuten + 59) / 60;
  if (stunden == 0) stunden = 1;
  return stunden * TARIF_RAPPEN_PRO_STUNDE;
}

// Offener Betrag (US-05, US-08)
int offenerBetrag(const Fahrzeug &f) {
  uint32_t t = jetzt();
  if (f.bezahltUm == 0) return gebuehrFuer(t - f.einfahrt);
  uint32_t seitZahlung = t - f.bezahltUm;
  if (seitZahlung <= KULANZ_SEK) return 0;
  return gebuehrFuer(seitZahlung);  // Nachberechnung der Zeit nach der Zahlung
}

// ======================================================================
// Schranken
// ======================================================================

void oeffne(Schranke &s) {
  s.ziel = WINKEL_OFFEN;
  s.offen = true;
  s.geoeffnetUm = millis();
  logMeldung(String("Schranke ") + s.name + " OFFEN");
}

void schliesse(Schranke &s) {
  s.ziel = WINKEL_ZU;
  s.offen = false;
  logMeldung(String("Schranke ") + s.name + " ZU");
}

// Bewegt den Servo schrittweise Richtung Ziel (nicht blockierend)
void bewegeServo(Schranke &s) {
  if (s.winkel == s.ziel || millis() - s.letzterSchritt < SERVO_SCHRITT_MS) return;
  s.letzterSchritt = millis();
  s.winkel += s.ziel > s.winkel ? 1 : -1;
  s.servo.write(s.winkel);
}

void pruefeAutoSchliessen(Schranke &s) {
  if (s.offen && millis() - s.geoeffnetUm >= SCHRANKE_OFFEN_MS) schliesse(s);
}

// ======================================================================
// Geschaeftslogik - jede Funktion liefert {ok, meldung}
// ======================================================================

// US-01, US-02, US-03, US-04
Ergebnis einfahren(const String &eingabe) {
  String kz = normalisiere(eingabe);
  if (kz == "") {
    fehlversuche++;
    logMeldung("Einfahrt abgelehnt, Kennzeichen ungueltig/nicht lesbar: '" + eingabe + "' (Fehlversuch " + fehlversuche + ")");
    String m = "Kennzeichen nicht erkannt oder ungueltig. Schranke bleibt geschlossen.";
    if (fehlversuche >= MAX_FEHLVERSUCHE) m += " Bitte Hilfe-Knopf druecken.";
    return {false, m};
  }
  if (finde(kz)) {
    logMeldung("KONFLIKT: " + kz + " ist bereits eingefahren");
    return {false, "Konflikt: " + kz + " ist bereits im Parkhaus. Schranke bleibt geschlossen."};
  }
  for (int i = 0; i < MAX_FAHRZEUGE; i++) {
    if (!fahrzeuge[i].belegt) {
      fahrzeuge[i].belegt = true;
      strncpy(fahrzeuge[i].kennzeichen, kz.c_str(), sizeof(fahrzeuge[i].kennzeichen) - 1);
      fahrzeuge[i].kennzeichen[sizeof(fahrzeuge[i].kennzeichen) - 1] = 0;
      fahrzeuge[i].einfahrt = jetzt();
      fahrzeuge[i].bezahltUm = 0;
      fahrzeuge[i].bezahltRappen = 0;
      fehlversuche = 0;
      logMeldung("Einfahrt registriert: " + kz + " um " + zeitText(fahrzeuge[i].einfahrt));
      oeffne(einfahrt);
      return {true, "Willkommen " + kz + "! Einfahrt um " + zeitText(fahrzeuge[i].einfahrt) + ". Schranke offen."};
    }
  }
  return {false, "Parkhaus voll. Schranke bleibt geschlossen."};
}

// US-03: Klingel/Hilfe
Ergebnis hilfeRufen() {
  logMeldung("!!! HILFE angefordert an der Einfahrt - Personal informiert !!!");
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_BUILTIN, i % 2 == 0);
    delay(150);
  }
  return {true, "Personal wurde informiert. Bitte warten."};
}

// US-05: nachvollziehbare Berechnung
Ergebnis gebuehr(const String &eingabe) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = kz == "" ? nullptr : finde(kz);
  if (!f) return {false, "Fahrzeug unbekannt, bitte Personal kontaktieren."};
  uint32_t t = jetzt();
  String m = "Kennzeichen: " + kz +
             "\nEinfahrt: " + zeitText(f->einfahrt) +
             "\nAktuelle Zeit: " + zeitText(t) +
             "\nAufenthaltsdauer: " + dauerText(t - f->einfahrt) +
             "\nTarif: CHF 2.00 pro angefangene Stunde";
  if (f->bezahltUm != 0) {
    m += "\nBereits bezahlt: " + chf(f->bezahltRappen) + " um " + zeitText(f->bezahltUm);
  }
  m += "\nOffener Betrag: " + chf(offenerBetrag(*f));
  return {true, m};
}

String quittung(const Fahrzeug &f, int betrag) {
  uint32_t t = jetzt();
  return "--- QUITTUNG ---\nKennzeichen: " + String(f.kennzeichen) +
         "\nEinfahrt: " + zeitText(f.einfahrt) +
         "\nAufenthaltsdauer: " + dauerText(t - f.einfahrt) +
         "\nBezahlt: " + chf(betrag) +
         "\nZeit: " + zeitText(t) +
         "\nAusfahrt innert 15 Minuten moeglich.";
}

// US-06, US-10. erfolgreich=false simuliert abgelehnte Zahlung
Ergebnis bezahlen(const String &eingabe, bool erfolgreich, const char *ort) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = kz == "" ? nullptr : finde(kz);
  if (!f) return {false, "Fahrzeug unbekannt, bitte Personal kontaktieren."};
  int betrag = offenerBetrag(*f);
  if (betrag == 0) return {true, "Nichts offen - " + kz + " ist bezahlt."};
  if (!erfolgreich) {
    logMeldung(String("Zahlung abgelehnt (") + ort + "): " + kz);
    return {false, "Zahlung fehlgeschlagen (Zahlungsmittel abgelehnt). Status bleibt 'offen'. Bitte erneut versuchen."};
  }
  f->bezahltUm = jetzt();
  f->bezahltRappen += betrag;
  logMeldung(String("Zahlung OK (") + ort + "): " + kz + " " + chf(betrag));
  return {true, quittung(*f, betrag)};
}

// US-07, US-08
Ergebnis ausfahren(const String &eingabe) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = kz == "" ? nullptr : finde(kz);
  if (!f) {
    logMeldung("Ausfahrt abgelehnt, unbekannt: '" + eingabe + "'");
    return {false, "Fahrzeug unbekannt, bitte Personal kontaktieren."};
  }
  int offen = offenerBetrag(*f);
  if (offen > 0) {
    logMeldung("Ausfahrt abgelehnt, offen: " + kz + " " + chf(offen));
    String grund = f->bezahltUm == 0 ? "Parkgebuehr nicht bezahlt" : "Kulanzzeit von 15 Minuten ueberschritten";
    return {false, grund + ". Offener Betrag: " + chf(offen) + ". Bitte an Kasse oder Automat bezahlen."};
  }
  logMeldung("Ausfahrt: " + kz);
  f->belegt = false;
  oeffne(ausfahrt);
  return {true, "Gute Fahrt, " + kz + "! Schranke offen."};
}

// US-09
String uebersichtText() {
  String s = "";
  int n = 0;
  for (int i = 0; i < MAX_FAHRZEUGE; i++) {
    if (!fahrzeuge[i].belegt) continue;
    n++;
    s += String(fahrzeuge[i].kennzeichen) + "  Einfahrt " + zeitText(fahrzeuge[i].einfahrt) +
         "  " + (fahrzeuge[i].bezahltUm ? "bezahlt" : "offen") + "\n";
  }
  return String(n) + " Fahrzeug(e) im Parkhaus\n" + s;
}

// Testhilfe (Annahme A3): Einfahrt bzw. Zahlung "vor X Minuten"
Ergebnis testhilfe(const String &eingabe, const String &was, long minuten) {
  String kz = normalisiere(eingabe);
  Fahrzeug *f = kz == "" ? nullptr : finde(kz);
  if (!f) return {false, "Fahrzeug unbekannt."};
  uint32_t t = jetzt() - minuten * 60;
  if (was == "einfahrt") {
    f->einfahrt = t;
  } else {
    if (f->bezahltUm == 0) return {false, kz + " hat noch nicht bezahlt."};
    f->bezahltUm = t;
  }
  logMeldung("Testhilfe: " + kz + " " + was + " auf vor " + minuten + " Min gesetzt");
  return {true, kz + ": " + was + " auf " + zeitText(t) + " gesetzt (vor " + minuten + " Min)."};
}

void zuruecksetzen() {
  for (int i = 0; i < MAX_FAHRZEUGE; i++) fahrzeuge[i].belegt = false;
  fehlversuche = 0;
  logMeldung("Testdaten zurueckgesetzt");
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

void sendeErgebnis(const Ergebnis &e) {
  server.send(200, "application/json",
              String("{\"ok\":") + (e.ok ? "true" : "false") + ",\"meldung\":\"" + jsonEscape(e.meldung) + "\"}");
}

void syncUhrAusRequest() {
  if (server.hasArg("t")) {
    uint32_t t = strtoul(server.arg("t").c_str(), nullptr, 10);
    if (t > 1600000000UL && (!uhrGesetzt || abs((long)(t - jetzt())) > 5)) setzeUhr(t);
  }
  if (server.hasArg("tz")) zeitzoneSek = server.arg("tz").toInt();
}

void setupWeb() {
  server.on("/", []() { server.send(200, "text/html", WEBPAGE); });
  server.on("/api/einfahrt", []() { syncUhrAusRequest(); sendeErgebnis(einfahren(server.arg("kz"))); });
  server.on("/api/ausfahrt", []() { syncUhrAusRequest(); sendeErgebnis(ausfahren(server.arg("kz"))); });
  server.on("/api/hilfe", []() { syncUhrAusRequest(); sendeErgebnis(hilfeRufen()); });
  server.on("/api/gebuehr", []() { syncUhrAusRequest(); sendeErgebnis(gebuehr(server.arg("kz"))); });
  server.on("/api/zahlen", []() {
    syncUhrAusRequest();
    sendeErgebnis(bezahlen(server.arg("kz"), server.arg("ok") != "0", server.arg("ort").c_str()));
  });
  server.on("/api/uebersicht", []() { syncUhrAusRequest(); sendeErgebnis({true, uebersichtText()}); });
  server.on("/api/testhilfe", []() {
    syncUhrAusRequest();
    sendeErgebnis(testhilfe(server.arg("kz"), server.arg("was"), server.arg("min").toInt()));
  });
  server.on("/api/reset", []() { zuruecksetzen(); sendeErgebnis({true, "Testdaten zurueckgesetzt."}); });
  server.on("/api/status", []() {
    syncUhrAusRequest();
    server.send(200, "application/json",
                String("{\"einfahrt\":") + (einfahrt.offen ? "true" : "false") +
                ",\"ausfahrt\":" + (ausfahrt.offen ? "true" : "false") + "}");
  });
  server.begin();
}

// ======================================================================
// Serieller Monitor
// ======================================================================

void druckeErgebnis(const Ergebnis &e) {
  Serial.println(e.ok ? "OK:" : "FEHLER:");
  Serial.println(e.meldung);
  Serial.println();
}

void druckeHilfe() {
  Serial.println("Befehle (Kennzeichen z.B. ZH 123456):");
  Serial.println("  ein <KZ>              Einfahrt (Kamera-Simulation)");
  Serial.println("  aus <KZ>              Ausfahrt (Kamera-Simulation)");
  Serial.println("  hilfe                 Hilfe-/Klingelknopf an der Einfahrt");
  Serial.println("  gebuehr <KZ>          Gebuehr anzeigen");
  Serial.println("  zahlen <KZ>           Bezahlen (erfolgreich)");
  Serial.println("  ablehnen <KZ>         Zahlung fehlgeschlagen simulieren");
  Serial.println("  liste                 Uebersicht parkierter Fahrzeuge");
  Serial.println("  test einfahrt <MIN> <KZ>  Einfahrt auf vor MIN Minuten setzen");
  Serial.println("  test zahlung <MIN> <KZ>   Zahlung auf vor MIN Minuten setzen");
  Serial.println("  reset                 Alle Daten loeschen");
  Serial.println("  uhr <EPOCH>           Uhrzeit setzen (Unix-Sekunden)");
  if (wlanAktiv) {
    Serial.print("Web: WLAN '");
    Serial.print(AP_SSID);
    Serial.print("' / '");
    Serial.print(AP_PASS);
    Serial.print("' -> http://");
    Serial.println(WiFi.softAPIP());
  }
  Serial.println();
}

void verarbeiteBefehl(String zeile) {
  zeile.trim();
  if (zeile == "") return;
  int sp = zeile.indexOf(' ');
  String cmd = sp < 0 ? zeile : zeile.substring(0, sp);
  String rest = sp < 0 ? "" : zeile.substring(sp + 1);
  cmd.toLowerCase();
  rest.trim();

  if (cmd == "ein") druckeErgebnis(einfahren(rest));
  else if (cmd == "aus") druckeErgebnis(ausfahren(rest));
  else if (cmd == "hilfe") druckeErgebnis(hilfeRufen());
  else if (cmd == "gebuehr") druckeErgebnis(gebuehr(rest));
  else if (cmd == "zahlen") druckeErgebnis(bezahlen(rest, true, "Kasse"));
  else if (cmd == "ablehnen") druckeErgebnis(bezahlen(rest, false, "Kasse"));
  else if (cmd == "liste") Serial.println(uebersichtText());
  else if (cmd == "reset") zuruecksetzen();
  else if (cmd == "uhr") { setzeUhr(strtoul(rest.c_str(), nullptr, 10)); Serial.println("Uhr: " + zeitText(jetzt())); }
  else if (cmd == "test") {
    // test <einfahrt|zahlung> <minuten> <KZ>
    int a = rest.indexOf(' ');
    int b = rest.indexOf(' ', a + 1);
    if (a < 0 || b < 0) { Serial.println("Syntax: test einfahrt|zahlung <MIN> <KZ>"); return; }
    druckeErgebnis(testhilfe(rest.substring(b + 1), rest.substring(0, a), rest.substring(a + 1, b).toInt()));
  }
  else druckeHilfe();
}

// ======================================================================
// Setup / Loop
// ======================================================================

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  // Zuerst WLAN starten (Stromspitze), dann die Servos nacheinander einschalten
  if (rp2040.isPicoW()) {
    WiFi.mode(WIFI_AP);
    wlanAktiv = WiFi.softAP(AP_SSID, AP_PASS);
    if (wlanAktiv) setupWeb();
  }
  delay(500);
  einfahrt.servo.attach(einfahrt.pin, 500, 2500, WINKEL_ZU);
  delay(800);
  ausfahrt.servo.attach(ausfahrt.pin, 500, 2500, WINKEL_ZU);
  delay(800);
  Serial.println("\n=== Parksystem M450 bereit ===");
  druckeHilfe();
}

void loop() {
  if (wlanAktiv) server.handleClient();

  static String zeile = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      verarbeiteBefehl(zeile);
      zeile = "";
    } else {
      zeile += c;
    }
  }

  bewegeServo(einfahrt);
  bewegeServo(ausfahrt);
  pruefeAutoSchliessen(einfahrt);
  pruefeAutoSchliessen(ausfahrt);
}
