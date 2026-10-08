#!/usr/bin/env python3
"""
ui_sim.py - esegue l'interfaccia dell'app su PC e stampa le schermate.

Compila main.cpp con un switch.h finto, avvia il server di prova, preme i
tasti indicati in uno scenario e ricostruisce lo schermo 80x45 dalle sequenze
ANSI. Ogni "SNAP" nello scenario stampa la schermata in quel momento
(le righe evidenziate sono marcate con '>' a sinistra); alla fine stampa anche
la schermata rimasta alla chiusura dell'app.

Uso: python tests/ui_sim/ui_sim.py <scenario.txt> [--config file.json] [--curl-include DIR]
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
W, H = 80, 45


class Screen:
    def __init__(self):
        self.clear()
        self.row, self.col, self.rev = 1, 1, False

    def clear(self):
        self.cells = [[" "] * (W + 2) for _ in range(H + 2)]
        self.revrow = [False] * (H + 2)

    def put(self, ch):
        if ch == "\n":
            self.row, self.col = self.row + 1, 1
            return
        if 1 <= self.row <= H and 1 <= self.col <= W:
            self.cells[self.row][self.col] = ch
            if self.rev and ch != " ":
                self.revrow[self.row] = True
        self.col += 1

    def feed(self, data, snaps):
        i = 0
        while i < len(data):
            c = data[i]
            if c == "\x1b":
                if data.startswith("\x1b]SNAP\x07", i):
                    snaps.append(self.render())
                    i += len("\x1b]SNAP\x07")
                    continue
                if data.startswith("\x1b]EVENT ", i):
                    end = data.index("\x07", i)
                    snaps.append("*** " + data[i + 8:end] + " ***")
                    i = end + 1
                    continue
                m = re.match(r"\x1b\[([0-9;]*)([A-Za-z])", data[i:])
                if not m:
                    i += 1
                    continue
                args = [int(a) if a else 0 for a in m.group(1).split(";")] if m.group(1) else []
                cmd = m.group(2)
                if cmd == "J":
                    self.clear()
                elif cmd == "H":
                    self.row = args[0] if args else 1
                    self.col = args[1] if len(args) > 1 else 1
                elif cmd == "K":
                    for x in range(self.col, W + 1):
                        if 1 <= self.row <= H:
                            self.cells[self.row][x] = " "
                    if 1 <= self.row <= H and self.col == 1:
                        self.revrow[self.row] = False
                elif cmd == "m":
                    for a in args or [0]:
                        if a == 7:
                            self.rev = True
                        elif a == 0:
                            self.rev = False
                i += len(m.group(0))
                continue
            self.put(c)
            i += 1

    def render(self):
        lines = []
        for r in range(1, H + 1):
            text = "".join(self.cells[r][1:W + 1]).rstrip()
            lines.append(("> " if self.revrow[r] else "  ") + text)
        return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("--config")
    ap.add_argument("--curl-include", default="/usr/include")
    ap.add_argument("--port", type=int, default=18766)
    ap.add_argument("--prepare", help="script shell da eseguire nella cartella di lavoro prima di avviare")
    a = ap.parse_args()

    work = tempfile.mkdtemp()
    try:
        exe = os.path.join(work, "app")
        libcurl = (glob.glob("/usr/lib/*/libcurl.so.4") or ["-lcurl"])[0]
        subprocess.check_call(["gcc", "-c", "-O1", "-o", os.path.join(work, "cJSON.o"),
                               os.path.join(PROJ, "source", "cJSON.c")])
        subprocess.check_call(["g++", "-std=gnu++17", "-O1", "-Wall", "-fno-exceptions", "-fno-rtti",
                               "-I" + HERE, "-I" + os.path.join(PROJ, "source"), "-I" + a.curl_include,
                               "-o", exe] +
                              [os.path.join(PROJ, "source", f) for f in ("main.cpp", "core.cpp", "mega.cpp", "pak.cpp", "net.cpp", "util.cpp")] +
                              [os.path.join(work, "cJSON.o"), libcurl])
        www = os.path.join(work, "www")
        os.makedirs(www)
        server = subprocess.Popen([sys.executable, os.path.join(PROJ, "tests", "server.py"), www, str(a.port)])
        time.sleep(1)
        run = os.path.join(work, "run")
        appdir = os.path.join(run, "sdmc:", "switch", "nst-pak-manager")
        os.makedirs(appdir)
        if a.config:
            cfg = open(a.config).read().replace("{PORT}", str(a.port))
            open(os.path.join(appdir, "config.json"), "w").write(cfg)
        if a.prepare:
            subprocess.check_call(["sh", "-c", a.prepare], cwd=run)
        env = dict(os.environ, SIM_KEYS=os.path.abspath(a.scenario))
        try:
            out = subprocess.run([exe], cwd=run, env=env, capture_output=True, timeout=120)
        finally:
            server.terminate()
        screen, snaps = Screen(), []
        screen.feed(out.stdout.decode("utf-8", "replace"), snaps)
        for n, s in enumerate(snaps, 1):
            print(f"===== SNAP {n} " + "=" * 60)
            print(s)
        print("===== SCHERMO ALLA CHIUSURA " + "=" * 46)
        print(screen.render())
        if out.stderr:
            print("STDERR:", out.stderr.decode(errors="replace"))
        print("===== FILE SU SD (dopo) " + "=" * 50)
        for root, dirs, files in os.walk(os.path.join(run, "sdmc:")):
            for f in sorted(files):
                p = os.path.join(root, f)
                print(os.path.relpath(p, run), os.path.getsize(p))
        print("exit code:", out.returncode)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
