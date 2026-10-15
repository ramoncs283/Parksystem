/*
  Schranken-Controller M450 - Raspberry Pi Pico (Arduino, Core: earlephilhower/arduino-pico)

  Der Pico steuert nur die beiden Schranken. Die ganze Logik (Kennzeichen, Gebuehren,
  Bezahlung, Weboberflaeche) laeuft auf dem Mac (mac-app/parksystem.py) und schickt
  ueber USB (115200 Baud) einfache Befehle:

    E OPEN / E CLOSE   Einfahrt (gruene Schranke, GP8)
    A OPEN / A CLOSE   Ausfahrt (rote Schranke, GP9)
    PING               Antwort: PONG
    STATUS             Antwort: STATUS E=<OPEN|CLOSED> A=<OPEN|CLOSED>

  Schutzmechanismen:
    - Servos fahren langsam (weniger Anlaufstrom, kein Brownout ueber USB)
    - Servos werden nur waehrend der Bewegung angesteuert (kein Dauerstrom im Stillstand)
    - Watchdog startet den Pico neu, falls er haengen bleibt
    - Offene Schranke schliesst nach 15 s selbst, falls der Mac den Befehl nicht mehr schickt
    - Zu lange Befehlszeilen werden verworfen
*/

#include <Servo.h>

const int PIN_SERVO_EINFAHRT = 8;   // gruene Schranke
const int PIN_SERVO_AUSFAHRT = 9;   // rote Schranke
const int WINKEL_ZU = 90;
const int WINKEL_OFFEN = 0;
const unsigned long SERVO_SCHRITT_MS = 15;        // 90 Grad in ca. 1.4 s
const unsigned long FAILSAFE_SCHLIESSEN_MS = 15000;
// Servo nach Erreichen der Position abschalten: kein Haltestrom, falls der Arm irgendwo ansteht
const unsigned long SERVO_AUS_NACH_MS = 400;
const unsigned int MAX_ZEILE = 32;

struct Schranke {
  Servo servo;
  int pin;
  char id;
  int winkel;
  int ziel;
  unsigned long letzterSchritt;
  unsigned long geoeffnetUm;
};

Schranke einfahrt = {Servo(), PIN_SERVO_EINFAHRT, 'E', WINKEL_ZU, WINKEL_ZU, 0, 0};
Schranke ausfahrt = {Servo(), PIN_SERVO_AUSFAHRT, 'A', WINKEL_ZU, WINKEL_ZU, 0, 0};

char zeile[MAX_ZEILE + 1];
unsigned int zeilenLaenge = 0;
bool zeileZuLang = false;

// Nie blockieren: nur senden, wenn der Mac zuhoert und Platz im Puffer ist
void antworte(const String &s) {
  if (Serial && Serial.availableForWrite() > (int)s.length() + 2) Serial.println(s);
}

bool istOffen(const Schranke &s) {
  return s.ziel == WINKEL_OFFEN;
}

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

void setze(Schranke &s, bool offen) {
  s.ziel = offen ? WINKEL_OFFEN : WINKEL_ZU;
  if (offen) s.geoeffnetUm = millis();
  antworte(String("OK ") + s.id + (offen ? " OPEN" : " CLOSE"));
}

void verarbeite(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd == "") return;
  if (cmd == "PING") antworte("PONG");
  else if (cmd == "STATUS")
    antworte(String("STATUS E=") + (istOffen(einfahrt) ? "OPEN" : "CLOSED") +
             " A=" + (istOffen(ausfahrt) ? "OPEN" : "CLOSED"));
  else if (cmd == "E OPEN") setze(einfahrt, true);
  else if (cmd == "E CLOSE") setze(einfahrt, false);
  else if (cmd == "A OPEN") setze(ausfahrt, true);
  else if (cmd == "A CLOSE") setze(ausfahrt, false);
  else antworte("ERR " + cmd);
}

void setup() {
  Serial.begin(115200);
  rp2040.wdt_begin(4000);

  // Servos nacheinander kurz in die Zu-Position fahren und wieder abschalten,
  // damit die USB-Versorgung nicht einbricht
  delay(300);
  einfahrt.servo.attach(einfahrt.pin, 500, 2500, WINKEL_ZU);
  delay(600);
  einfahrt.servo.detach();
  rp2040.wdt_reset();
  ausfahrt.servo.attach(ausfahrt.pin, 500, 2500, WINKEL_ZU);
  delay(600);
  ausfahrt.servo.detach();
  rp2040.wdt_reset();

  antworte("READY");
}

void loop() {
  rp2040.wdt_reset();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!zeileZuLang) {
        zeile[zeilenLaenge] = 0;
        verarbeite(String(zeile));
      }
      zeilenLaenge = 0;
      zeileZuLang = false;
    } else if (zeilenLaenge < MAX_ZEILE) {
      zeile[zeilenLaenge++] = c;
    } else {
      zeileZuLang = true;
    }
  }

  bewegeServo(einfahrt);
  bewegeServo(ausfahrt);

  if (istOffen(einfahrt) && millis() - einfahrt.geoeffnetUm > FAILSAFE_SCHLIESSEN_MS) setze(einfahrt, false);
  if (istOffen(ausfahrt) && millis() - ausfahrt.geoeffnetUm > FAILSAFE_SCHLIESSEN_MS) setze(ausfahrt, false);
}
