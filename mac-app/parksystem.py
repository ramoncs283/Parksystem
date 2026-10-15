#!/usr/bin/env python3
"""
Parksystem M450 - Mac-Anwendung

Enthaelt die ganze Logik (Kennzeichen, Gebuehren, Bezahlung, Uebersicht) und die
Weboberflaeche. Der Pico (Schranken/Schranken.ino) bekommt ueber USB nur Befehle
wie "E OPEN" oder "A CLOSE".

Start:  python3 mac-app/parksystem.py   ->  http://localhost:8080
Nur Python-Standardbibliothek, keine Installation noetig.
"""

import glob
import json
import os
import re
import select
import sys
import termios
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HIER = os.path.dirname(os.path.abspath(__file__))
DATENDATEI = os.path.join(HIER, "daten.json")
LOGDATEI = os.path.join(HIER, "parksystem.log")
PORT = 8080

# Regeln gemaess Testkonzept (Annahmen A1-A3)
TARIF_RAPPEN_PRO_STUNDE = 200
KULANZ_SEK = 15 * 60
SCHRANKE_OFFEN_SEK = 5
MAX_FEHLVERSUCHE = 3
KAPAZITAET = 50
KENNZEICHEN_MUSTER = re.compile(r"^([A-Z]{2})(\d{1,6})$")


def log(meldung):
    zeile = f"[{time.strftime('%H:%M:%S')}] {meldung}"
    print(zeile, flush=True)
    with open(LOGDATEI, "a", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%d ") + zeile + "\n")


def zeit_text(t):
    return time.strftime("%H:%M:%S", time.localtime(t))


def dauer_text(sek):
    sek = max(0, int(sek))
    return f"{sek // 3600} h {(sek // 60) % 60:02d} min"


def chf(rappen):
    return f"CHF {rappen // 100}.{rappen % 100:02d}"


# ======================================================================
# Verbindung zum Pico
# ======================================================================

class Pico:
    """Haelt die USB-Verbindung zum Pico, verbindet bei Abbruch automatisch neu."""

    def __init__(self, bei_verbindung):
        self.fd = None
        self.port = None
        self.letzte_antwort = 0
        self.lock = threading.Lock()
        self.bei_verbindung = bei_verbindung
        threading.Thread(target=self._lauf, daemon=True).start()

    @property
    def verbunden(self):
        return self.fd is not None and time.time() - self.letzte_antwort < 5

    def senden(self, befehl):
        with self.lock:
            if self.fd is None:
                return False
            try:
                os.write(self.fd, (befehl + "\n").encode())
                return True
            except (BlockingIOError, OSError):
                self._trennen("Senden fehlgeschlagen")
                return False

    def _oeffnen(self):
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if not ports:
            return
        port = ports[0]
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            attr = termios.tcgetattr(fd)
            attr[0] = 0                                   # iflag
            attr[1] = 0                                   # oflag
            attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attr[3] = 0                                   # lflag (raw)
            attr[4] = attr[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, attr)
        except OSError:
            return
        with self.lock:
            self.fd, self.port = fd, port
        self.letzte_antwort = time.time()
        log(f"Pico verbunden ({port})")
        self.senden("PING")
        self.bei_verbindung()

    def _trennen(self, grund):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None
            log(f"Pico getrennt: {grund}")

    def _lauf(self):
        puffer = b""
        letzter_ping = 0
        while True:
            if self.fd is None:
                self._oeffnen()
                if self.fd is None:
                    time.sleep(1)
                    continue
            try:
                bereit, _, _ = select.select([self.fd], [], [], 0.2)
                if bereit:
                    daten = os.read(self.fd, 4096)
                    if not daten:
                        raise OSError("keine Daten")
                    puffer += daten
                    while b"\n" in puffer:
                        zeile, puffer = puffer.split(b"\n", 1)
                        zeile = zeile.decode(errors="replace").strip()
                        if zeile:
                            self.letzte_antwort = time.time()
                            if zeile == "READY":
                                log("Pico neu gestartet - Schranken werden neu gesetzt")
                                self.bei_verbindung()
            except (BlockingIOError, InterruptedError):
                pass
            except OSError as e:
                with self.lock:
                    self._trennen(str(e))
                puffer = b""
                time.sleep(1)
                continue
            if time.time() - letzter_ping > 2:
                letzter_ping = time.time()
                self.senden("PING")
            if self.fd is not None and time.time() - self.letzte_antwort > 8:
                with self.lock:
                    self._trennen("keine Antwort")


# ======================================================================
# Parksystem-Logik
# ======================================================================

class Parksystem:
    def __init__(self):
        self.lock = threading.RLock()
        self.fahrzeuge = {}   # kz -> {"einfahrt": t, "bezahlt_um": t|None, "bezahlt_rappen": int}
        self.fehlversuche = 0
        self.schranken = {"E": 0.0, "A": 0.0}   # Zeitpunkt des Oeffnens, 0 = zu
        self._laden()
        self.pico = Pico(self._schranken_neu_senden)
        threading.Thread(target=self._auto_schliessen, daemon=True).start()

    # ---------- Speicherung ----------
    def _laden(self):
        try:
            with open(DATENDATEI, encoding="utf-8") as f:
                self.fahrzeuge = json.load(f)
            log(f"{len(self.fahrzeuge)} Fahrzeug(e) aus {os.path.basename(DATENDATEI)} geladen")
        except (OSError, ValueError):
            self.fahrzeuge = {}

    def _speichern(self):
        tmp = DATENDATEI + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.fahrzeuge, f, indent=2)
        os.replace(tmp, DATENDATEI)

    # ---------- Schranken ----------
    def _oeffne(self, s):
        self.schranken[s] = time.time()
        name = "Einfahrt" if s == "E" else "Ausfahrt"
        log(f"Schranke {name} OFFEN")
        if not self.pico.senden(f"{s} OPEN"):
            log("WARNUNG: Pico nicht verbunden, Schranke bewegt sich nicht")

    def _schliesse(self, s):
        self.schranken[s] = 0.0
        log(f"Schranke {'Einfahrt' if s == 'E' else 'Ausfahrt'} ZU")
        self.pico.senden(f"{s} CLOSE")

    def _schranken_neu_senden(self):
        for s, offen_seit in self.schranken.items():
            self.pico.senden(f"{s} {'OPEN' if offen_seit else 'CLOSE'}")

    def _auto_schliessen(self):
        while True:
            time.sleep(0.1)
            with self.lock:
                for s, offen_seit in self.schranken.items():
                    if offen_seit and time.time() - offen_seit >= SCHRANKE_OFFEN_SEK:
                        self._schliesse(s)

    # ---------- Regeln ----------
    @staticmethod
    def normalisiere(eingabe):
        kz = re.sub(r"\s+", "", (eingabe or "").upper())
        m = KENNZEICHEN_MUSTER.match(kz)
        return f"{m.group(1)} {m.group(2)}" if m else None

    @staticmethod
    def gebuehr_fuer(sek):
        minuten = max(0, int(sek)) // 60
        stunden = max(1, (minuten + 59) // 60)
        return stunden * TARIF_RAPPEN_PRO_STUNDE

    def offener_betrag(self, f):
        jetzt = time.time()
        if f["bezahlt_um"] is None:
            return self.gebuehr_fuer(jetzt - f["einfahrt"])
        seit_zahlung = jetzt - f["bezahlt_um"]
        if seit_zahlung <= KULANZ_SEK:
            return 0
        return self.gebuehr_fuer(seit_zahlung)

    def _finde(self, eingabe):
        kz = self.normalisiere(eingabe)
        return kz, (self.fahrzeuge.get(kz) if kz else None)

    # ---------- Aktionen (US-01 bis US-10) ----------
    def einfahren(self, eingabe):
        with self.lock:
            kz = self.normalisiere(eingabe)
            if not kz:
                self.fehlversuche += 1
                log(f"Einfahrt abgelehnt, Kennzeichen ungueltig/nicht lesbar: '{eingabe}' (Fehlversuch {self.fehlversuche})")
                m = "Kennzeichen nicht erkannt oder ungültig. Schranke bleibt geschlossen."
                if self.fehlversuche >= MAX_FEHLVERSUCHE:
                    m += " Bitte Hilfe-Knopf drücken."
                return False, m
            if kz in self.fahrzeuge:
                log(f"KONFLIKT: {kz} ist bereits eingefahren")
                return False, f"Konflikt: {kz} ist bereits im Parkhaus. Schranke bleibt geschlossen."
            if len(self.fahrzeuge) >= KAPAZITAET:
                return False, "Parkhaus voll. Schranke bleibt geschlossen."
            jetzt = time.time()
            self.fahrzeuge[kz] = {"einfahrt": jetzt, "bezahlt_um": None, "bezahlt_rappen": 0}
            self.fehlversuche = 0
            self._speichern()
            log(f"Einfahrt registriert: {kz} um {zeit_text(jetzt)}")
            self._oeffne("E")
            return True, f"Willkommen {kz}! Einfahrt um {zeit_text(jetzt)}. Schranke offen."

    def hilfe(self):
        log("!!! HILFE angefordert an der Einfahrt - Personal informiert !!!")
        return True, "Personal wurde informiert. Bitte warten."

    def gebuehr(self, eingabe):
        with self.lock:
            kz, f = self._finde(eingabe)
            if not f:
                return False, "Fahrzeug unbekannt, bitte Personal kontaktieren."
            jetzt = time.time()
            m = (f"Kennzeichen: {kz}\nEinfahrt: {zeit_text(f['einfahrt'])}\n"
                 f"Aktuelle Zeit: {zeit_text(jetzt)}\n"
                 f"Aufenthaltsdauer: {dauer_text(jetzt - f['einfahrt'])}\n"
                 f"Tarif: CHF 2.00 pro angefangene Stunde")
            if f["bezahlt_um"] is not None:
                m += f"\nBereits bezahlt: {chf(f['bezahlt_rappen'])} um {zeit_text(f['bezahlt_um'])}"
            m += f"\nOffener Betrag: {chf(self.offener_betrag(f))}"
            return True, m

    def bezahlen(self, eingabe, erfolgreich, ort):
        with self.lock:
            kz, f = self._finde(eingabe)
            if not f:
                return False, "Fahrzeug unbekannt, bitte Personal kontaktieren."
            betrag = self.offener_betrag(f)
            if betrag == 0:
                return True, f"Nichts offen - {kz} ist bezahlt."
            if not erfolgreich:
                log(f"Zahlung abgelehnt ({ort}): {kz}")
                return False, "Zahlung fehlgeschlagen (Zahlungsmittel abgelehnt). Status bleibt 'offen'. Bitte erneut versuchen."
            jetzt = time.time()
            f["bezahlt_um"] = jetzt
            f["bezahlt_rappen"] += betrag
            self._speichern()
            log(f"Zahlung OK ({ort}): {kz} {chf(betrag)}")
            return True, (f"--- QUITTUNG ---\nKennzeichen: {kz}\nEinfahrt: {zeit_text(f['einfahrt'])}\n"
                          f"Aufenthaltsdauer: {dauer_text(jetzt - f['einfahrt'])}\nBezahlt: {chf(betrag)}\n"
                          f"Zeit: {zeit_text(jetzt)}\nAusfahrt innert 15 Minuten möglich.")

    def ausfahren(self, eingabe):
        with self.lock:
            kz, f = self._finde(eingabe)
            if not f:
                log(f"Ausfahrt abgelehnt, unbekannt: '{eingabe}'")
                return False, "Fahrzeug unbekannt, bitte Personal kontaktieren."
            offen = self.offener_betrag(f)
            if offen > 0:
                log(f"Ausfahrt abgelehnt, offen: {kz} {chf(offen)}")
                grund = ("Parkgebühr nicht bezahlt" if f["bezahlt_um"] is None
                         else "Kulanzzeit von 15 Minuten überschritten")
                return False, f"{grund}. Offener Betrag: {chf(offen)}. Bitte an Kasse oder Automat bezahlen."
            del self.fahrzeuge[kz]
            self._speichern()
            log(f"Ausfahrt: {kz}")
            self._oeffne("A")
            return True, f"Gute Fahrt, {kz}! Schranke offen."

    def uebersicht(self):
        with self.lock:
            zeilen = [f"{kz}  Einfahrt {zeit_text(f['einfahrt'])}  {'bezahlt' if f['bezahlt_um'] else 'offen'}"
                      for kz, f in sorted(self.fahrzeuge.items(), key=lambda x: x[1]["einfahrt"])]
            return True, f"{len(zeilen)} Fahrzeug(e) im Parkhaus\n" + "\n".join(zeilen)

    def testhilfe(self, eingabe, was, minuten):
        with self.lock:
            kz, f = self._finde(eingabe)
            if not f:
                return False, "Fahrzeug unbekannt."
            minuten = max(0, min(int(minuten), 100000))
            t = time.time() - minuten * 60
            if was == "einfahrt":
                f["einfahrt"] = t
            else:
                if f["bezahlt_um"] is None:
                    return False, f"{kz} hat noch nicht bezahlt."
                f["bezahlt_um"] = t
            self._speichern()
            log(f"Testhilfe: {kz} {was} auf vor {minuten} Min gesetzt")
            return True, f"{kz}: {was} auf {zeit_text(t)} gesetzt (vor {minuten} Min)."

    def zuruecksetzen(self):
        with self.lock:
            self.fahrzeuge = {}
            self.fehlversuche = 0
            self._speichern()
            log("Testdaten zurückgesetzt")
            return True, "Testdaten zurückgesetzt."

    def status(self):
        return {"einfahrt": bool(self.schranken["E"]), "ausfahrt": bool(self.schranken["A"]),
                "pico": self.pico.verbunden}


# ======================================================================
# Webserver
# ======================================================================

system = None


class Handler(BaseHTTPRequestHandler):
    def _json(self, daten):
        body = json.dumps(daten).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        kz = q.get("kz", "")
        aktionen = {
            "/api/einfahrt": lambda: system.einfahren(kz),
            "/api/ausfahrt": lambda: system.ausfahren(kz),
            "/api/hilfe": system.hilfe,
            "/api/gebuehr": lambda: system.gebuehr(kz),
            "/api/zahlen": lambda: system.bezahlen(kz, q.get("ok") != "0", q.get("ort", "Kasse")),
            "/api/uebersicht": system.uebersicht,
            "/api/testhilfe": lambda: system.testhilfe(kz, q.get("was", ""), q.get("min", "0") or "0"),
            "/api/reset": system.zuruecksetzen,
        }
        try:
            if url.path in ("/", "/index.html"):
                with open(os.path.join(HIER, "index.html"), "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif url.path == "/api/status":
                self._json(system.status())
            elif url.path in aktionen:
                ok, meldung = aktionen[url.path]()
                self._json({"ok": ok, "meldung": meldung})
            else:
                self.send_error(404)
        except ValueError:
            self._json({"ok": False, "meldung": "Ungültige Eingabe."})

    def log_message(self, *args):
        pass  # keine Zugriffslogs in der Konsole


def main():
    global system
    system = Parksystem()
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    url = f"http://localhost:{PORT}"
    log(f"Parksystem läuft auf {url}  (Beenden mit Ctrl+C)")
    if "--kein-browser" not in sys.argv:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        system.pico.senden("E CLOSE")
        system.pico.senden("A CLOSE")
        log("Parksystem beendet")


if __name__ == "__main__":
    main()
