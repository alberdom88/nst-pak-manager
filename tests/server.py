#!/usr/bin/env python3
"""Server di prova per i test: elenco cartella, manifest, finta API Google Drive."""
import json
import os
import sys
import time
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

ROOT = sys.argv[1]          # cartella con files/ e manifest.json
PORT = int(sys.argv[2])
KEY = "TESTKEY"

def pak_bytes(tag, size):
    body = (tag.encode() * (size // len(tag) + 1))[: size - 4]
    return b"IGA\x1a" + body

def level_pak(game, level):
    """.pak valido e non compresso con un livello (packages/generated/maps/...)."""
    import struct
    root = "temporary/mack/data/win64/output/"
    files = [("packages/generated/maps/%s/%s/%s_pkg.igz" % (game, level, level), b"\x01ZGI" + b"P" * 300),
             ("maps/%s/%s/%s_Terrain.igz" % (game, level, level), b"\x01ZGI" + b"T" * 900)]
    n = len(files)
    start = (0x38 + 20 * n + 0x7FF) & ~0x7FF
    body, offs = b"", []
    for _, data in files:
        offs.append(start + len(body))
        body += data + b"\0" * (-len(data) % 0x800)
    names, ptr = b"", []
    for name, _ in files:
        ptr.append(4 * n + len(names))
        names += (root + name).encode() + b"\0" + name.encode() + b"\0" + b"\0" * 4
    table = struct.pack("<%dI" % n, *ptr) + names
    head = struct.pack("<10IQ2I", 0x1A414749, 11, 20 * n, n, 0x800, 0x7FFFFFFF, 0, 0, 0, 0,
                       start + len(body), len(table), 1)
    toc = struct.pack("<%dI" % n, *range(n)) + b"".join(
        struct.pack("<IiiI", offs[i], 0, len(files[i][1]), 0xFFFFFFFF) for i in range(n))
    blob = head + toc
    return blob + b"\0" * (start - len(blob)) + body + table

DRIVE = [  # (id, name, size, mime)
    ("id1", "L101_NSanityBeach.pak", 70000, "application/octet-stream"),
    ("id2", "update.pak", 5000, "application/octet-stream"),
    ("idf", "sottocartella", None, "application/vnd.google-apps.folder"),
    ("id3", "leggimi.txt", 10, "text/plain"),
    ("id4", "L102_Jungle.pak", 1234, "application/octet-stream"),
]

class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def log_message(self, *a):
        pass

    def send_bytes(self, code, data, ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        qs = parse_qs(u.query)
        if u.path == "/drive/v3/files":
            if qs.get("key", [""])[0] != KEY:
                return self.send_bytes(403, json.dumps({"error": {"code": 403, "message": "API key not valid"}}).encode(), "application/json")
            q = qs.get("q", [""])[0]
            if q != "'FOLDER123' in parents and trashed = false":
                return self.send_bytes(404, json.dumps({"error": {"code": 404, "message": "File not found: " + q}}).encode(), "application/json")
            page = qs.get("pageToken", [""])[0]
            items = [{"id": i, "name": n, "mimeType": m, **({"size": str(s)} if s else {})} for i, n, s, m in DRIVE]
            if page == "":
                body = {"nextPageToken": "P2", "files": items[:3]}
            elif page == "P2":
                body = {"files": items[3:]}
            else:
                return self.send_bytes(400, b"{}", "application/json")
            return self.send_bytes(200, json.dumps(body).encode(), "application/json")
        if u.path.startswith("/drive/v3/files/"):
            if qs.get("key", [""])[0] != KEY or qs.get("alt", [""])[0] != "media":
                return self.send_bytes(403, b"no")
            fid = u.path.rsplit("/", 1)[1]
            for i, n, s, m in DRIVE:
                if i == fid and s:
                    return self.send_bytes(200, pak_bytes("drive-" + n, s))
            return self.send_bytes(404, b"missing")
        if u.path == "/redirect":
            self.send_response(301)
            self.send_header("Location", "/files/")
            self.end_headers()
            return
        if u.path == "/fake/not_a_pak.pak":
            return self.send_bytes(200, b"<html>Sign in</html>", "text/html")
        if u.path == "/fake/slow.pak":
            self.send_response(200)
            self.send_header("Content-Length", str(10 * 1024 * 1024))
            self.end_headers()
            try:
                self.wfile.write(b"IGA\x1a")
                for _ in range(200):
                    self.wfile.write(b"x" * 4096)
                    self.wfile.flush()
                    time.sleep(0.02)
            except Exception:
                pass
            return
        return super().do_GET()

if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "files"), exist_ok=True)
    files = {"L101_NSanityBeach.pak": 70000, "update.pak": 3000, "Nome con spazi.pak": 2048}
    for n, s in files.items():
        with open(os.path.join(ROOT, "files", n), "wb") as f:
            f.write(pak_bytes("http-" + n, s))
    with open(os.path.join(ROOT, "files", "Custom_Level.pak"), "wb") as f:
        f.write(level_pak("Crash1", "Custom_Level"))
    with open(os.path.join(ROOT, "files", "note.txt"), "w") as f:
        f.write("non un pak")
    manifest = {"files": [
        {"name": "update.pak", "url": "files/update.pak", "size": 3000},
        {"name": "Nome con spazi.pak", "size": 2048},
        {"url": "http://127.0.0.1:%d/files/L101_NSanityBeach.pak" % PORT},
        {"name": "../evil.pak", "url": "files/update.pak"},
        {"name": "wrong_size.pak", "url": "files/update.pak", "size": 9999},
        {"name": "custom_v2.pak", "url": "files/update.pak", "target": "L101_NSanityBeach.pak"},
    ]}
    # "Nome con spazi.pak" senza url -> risolto rispetto alla cartella del manifest
    os.makedirs(os.path.join(ROOT, "m"), exist_ok=True)
    with open(os.path.join(ROOT, "m", "manifest.json"), "w") as f:
        json.dump(manifest, f)
    import shutil
    shutil.copy(os.path.join(ROOT, "files", "Nome con spazi.pak"), os.path.join(ROOT, "m", "Nome con spazi.pak"))
    os.makedirs(os.path.join(ROOT, "m", "files"), exist_ok=True)
    shutil.copy(os.path.join(ROOT, "files", "update.pak"), os.path.join(ROOT, "m", "files", "update.pak"))
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
