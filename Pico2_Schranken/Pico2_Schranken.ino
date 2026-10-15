/*
  Parksystem M450 - Pico 2 (Schranken)
  Raspberry Pi Pico W, Arduino (Core: earlephilhower/arduino-pico)

  Aufgaben (Testkonzept Kap. 1.2):
    - Verbindet sich mit dem WLAN "Parksystem" von Pico 1, feste IP 192.168.4.2
    - Weboberflaeche der Kamera-Simulation (Kennzeichen bei Ein- und Ausfahrt eingeben)
    - Prueft das Kennzeichenformat (US-03) und fragt Pico 1 bei Ein- und Ausfahrt an
    - Steuert beide Schranken:
        Einfahrt = gruene Schranke, Servo an GP8 (Pin 11)
        Ausfahrt = rote Schranke,   Servo an GP9 (Pin 12)

  Sicherheit:
    - Ist Pico 1 nicht erreichbar, bleiben beide Schranken geschlossen (TC-17)
    - Servos fahren langsam und sind im Stillstand abgeschaltet (kein Spannungseinbruch ueber USB)
    - Watchdog startet den Pico neu, falls er haengen bleibt
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Servo.h>
#include "webpage.h"

// ---------- Netzwerk ----------
const char *WLAN_SSID = "Parksystem";
const char *WLAN_PASS = "parkhaus123";
const IPAddress EIGENE_IP(192, 168, 4, 2);
const IPAddress KASSE_IP(192, 168, 4, 1);
const IPAddress SUBNETZ(255, 255, 255, 0);
const uint16_t KASSE_TIMEOUT_MS = 2000;
const unsigned long WLAN_NEU_VERBINDEN_MS = 10000;

// ---------- Schranken ----------
const int PIN_SERVO_EINFAHRT = 8;
const int PIN_SERVO_AUSFAHRT = 9;
const int WINKEL_ZU = 90;
const int WINKEL_OFFEN = 0;
const unsigned long SCHRANKE_OFFEN_MS = 5000;   // US-02: schliesst nach Durchfahrt automatisch
const unsigned long SERVO_SCHRITT_MS = 15;      // 90 Grad in ca. 1.4 s (innert 2 s offen)
const unsigned long SERVO_AUS_NACH_MS = 400;
const int MAX_FEHLVERSUCHE = 3;

struct Schranke {
  Servo servo;
  int pin;
  const char *name;
  int winkel;
  int ziel;
  unsigned long letzterSchritt;
  unsigned long geoeffnetUm;
};

struct Ergebnis {
  bool ok;
  String meldung;
};

Schranke einfahrt = {Servo(), PIN_SERVO_EINFAHRT, "Einfahrt", WINKEL_ZU, WINKEL_ZU, 0, 0};
Schranke ausfahrt = {Servo(), PIN_SERVO_AUSFAHRT, "Ausfahrt", WINKEL_ZU, WINKEL_ZU, 0, 0};

WebServer server(80);
int fehlversuche = 0;
bool kasseErreichbar = false;
unsigned long letzterWlanVersuch = 0;
unsigned long letzterPing = 0;

void logMeldung(const String &s) {
  if (Serial && Serial.availableForWrite() > (int)s.length() + 2) Serial.println(s);
}

// ======================================================================
// Schranken
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
// Kennzeichen und Kommunikation mit Pico 1
// ======================================================================

// A1: 2 Buchstaben + 1-6 Ziffern -> "ZH123456" (ohne Leerzeichen, fuer die URL), sonst ""
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
  return k;
}

// Fragt Pico 1 an. Antwort: erste Zeile "JA"/"NEIN", danach die Meldung.
Ergebnis frageKasse(const String &pfad) {
  if (WiFi.status() != WL_CONNECTED) {
    kasseErreichbar = false;
    return {false, "Keine Verbindung zur Kasse (Pico 1). Schranke bleibt geschlossen."};
  }
  // Uhrzeit des Browsers an die Kasse weitergeben
  String url = "http://" + KASSE_IP.toString() + pfad + "&t=" + server.arg("t") + "&tz=" + server.arg("tz");
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(KASSE_TIMEOUT_MS);
  Ergebnis e = {false, "Kasse (Pico 1) nicht erreichbar. Schranke bleibt geschlossen. Bitte Personal kontaktieren."};
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == 200) {
      String antwort = http.getString();
      int nl = antwort.indexOf('\n');
      String status = nl < 0 ? antwort : antwort.substring(0, nl);
      e = {status == "JA", nl < 0 ? "" : antwort.substring(nl + 1)};
      kasseErreichbar = true;
    } else {
      kasseErreichbar = false;
    }
    http.end();
  } else {
    kasseErreichbar = false;
  }
  return e;
}

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
  Ergebnis e = frageKasse("/intern/einfahrt?kz=" + kz);
  if (e.ok) {
    fehlversuche = 0;
    oeffne(einfahrt);
    e.meldung += " Schranke offen.";
  }
  return e;
}

// US-07, US-08
Ergebnis ausfahren(const String &eingabe) {
  String kz = normalisiere(eingabe);
  if (kz == "") return {false, "Fahrzeug unbekannt, bitte Personal kontaktieren."};
  Ergebnis e = frageKasse("/intern/ausfahrt?kz=" + kz);
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

void setupWeb() {
  server.on("/", []() { server.send(200, "text/html", WEBPAGE); });
  server.on("/api/einfahrt", []() { sendeJson(einfahren(server.arg("kz"))); });
  server.on("/api/ausfahrt", []() { sendeJson(ausfahren(server.arg("kz"))); });
  server.on("/api/hilfe", []() { sendeJson(hilfeRufen()); });
  server.on("/api/status", []() {
    if (millis() - letzterPing > 5000) {
      letzterPing = millis();
      frageKasse("/intern/ping?x=1");
    }
    server.send(200, "application/json",
                String("{\"einfahrt\":") + (istOffen(einfahrt) ? "true" : "false") +
                ",\"ausfahrt\":" + (istOffen(ausfahrt) ? "true" : "false") +
                ",\"kasse\":" + (WiFi.status() == WL_CONNECTED && kasseErreichbar ? "true" : "false") + "}");
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Nicht gefunden"); });
  server.begin();
}

void verbindeWlan() {
  letzterWlanVersuch = millis();
  WiFi.config(EIGENE_IP, KASSE_IP, KASSE_IP, SUBNETZ);
  WiFi.beginNoBlock(WLAN_SSID, WLAN_PASS);
}

// ======================================================================
// Setup / Loop
// ======================================================================

void setup() {
  Serial.begin(115200);
  rp2040.wdt_begin(8000);

  // Servos nacheinander kurz in die Zu-Position fahren und wieder abschalten
  delay(300);
  einfahrt.servo.attach(einfahrt.pin, 500, 2500, WINKEL_ZU);
  delay(600);
  einfahrt.servo.detach();
  rp2040.wdt_reset();
  ausfahrt.servo.attach(ausfahrt.pin, 500, 2500, WINKEL_ZU);
  delay(600);
  ausfahrt.servo.detach();
  rp2040.wdt_reset();

  WiFi.mode(WIFI_STA);
  verbindeWlan();
  setupWeb();
  logMeldung("Pico 2 (Schranken) bereit, Weboberflaeche: http://" + EIGENE_IP.toString());
}

void loop() {
  rp2040.wdt_reset();

  if (WiFi.status() != WL_CONNECTED) {
    kasseErreichbar = false;
    if (millis() - letzterWlanVersuch > WLAN_NEU_VERBINDEN_MS) verbindeWlan();
  } else {
    server.handleClient();
  }

  bewegeServo(einfahrt);
  bewegeServo(ausfahrt);
  pruefeAutoSchliessen(einfahrt);
  pruefeAutoSchliessen(ausfahrt);
}
