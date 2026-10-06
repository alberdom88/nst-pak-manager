#!/usr/bin/env python3
"""Server di prova per i test: elenco cartella, manifest, finta API di MEGA."""
import base64
import json
import os
import sys
import time
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

ROOT = sys.argv[1]          # cartella con files/ e manifest.json
PORT = int(sys.argv[2])

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

# ---------------------------------------------------------------- finta MEGA
# AES-128 (solo cifratura): chiavi dei nodi (ECB), attributi (CBC) e contenuti (CTR)
def _xt(x):
    return ((x << 1) ^ (0x1B if x & 0x80 else 0)) & 0xFF

_EXP, _LOG = [0] * 512, [0] * 256
_x = 1
for _i in range(255):
    _EXP[_i], _LOG[_x] = _x, _i
    _x ^= _xt(_x)
for _i in range(255, 512):
    _EXP[_i] = _EXP[_i - 255]
SBOX = []
for _x in range(256):
    inv = _EXP[255 - _LOG[_x]] if _x else 0
    s_ = inv
    for _i in range(1, 5):
        s_ ^= ((inv << _i) | (inv >> (8 - _i))) & 0xFF
    SBOX.append(s_ ^ 0x63)

def aes_keys(key):
    rk = list(key)
    rcon = [1, 2, 4, 8, 16, 32, 64, 128, 0x1B, 0x36]
    for i in range(4, 44):
        t = rk[4 * (i - 1):4 * i]
        if i % 4 == 0:
            t = [SBOX[t[1]] ^ rcon[i // 4 - 1], SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
        rk += [rk[4 * (i - 4) + j] ^ t[j] for j in range(4)]
    return rk

def aes_block(rk, block):
    s_ = [block[i] ^ rk[i] for i in range(16)]
    for rnd in range(1, 11):
        t = [SBOX[s_[r + 4 * ((c + r) % 4)]] for c in range(4) for r in range(4)]
        if rnd < 10:
            for c in range(4):
                a0, a1, a2, a3 = t[4 * c:4 * c + 4]
                al = a0 ^ a1 ^ a2 ^ a3
                t[4 * c:4 * c + 4] = [a0 ^ al ^ _xt(a0 ^ a1), a1 ^ al ^ _xt(a1 ^ a2),
                                      a2 ^ al ^ _xt(a2 ^ a3), a3 ^ al ^ _xt(a3 ^ a0)]
        s_ = [t[i] ^ rk[16 * rnd + i] for i in range(16)]
    return bytes(s_)

assert aes_block(aes_keys(bytes(range(16))), bytes.fromhex("00112233445566778899aabbccddeeff")).hex() == \
    "69c4e0d86a7b0430d8cdb78070b4c55a"

def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()

def ecb(key, data):
    rk = aes_keys(key)
    return b"".join(aes_block(rk, data[i:i + 16]) for i in range(0, len(data), 16))

def cbc(key, data):
    rk, prev, out = aes_keys(key), bytes(16), b""
    for i in range(0, len(data), 16):
        prev = aes_block(rk, bytes(a ^ b for a, b in zip(data[i:i + 16], prev)))
        out += prev
    return out

def ctr(key, iv, data):
    rk, out = aes_keys(key), bytearray()
    counter = int.from_bytes(iv, "big")
    for i in range(0, len(data), 16):
        stream = aes_block(rk, counter.to_bytes(16, "big"))
        out += bytes(a ^ b for a, b in zip(data[i:i + 16], stream))
        counter += 1
    return bytes(out)

def attrs(name, key):
    raw = b"MEGA" + json.dumps({"n": name}).encode()
    return b64(cbc(key, raw + b"\0" * (-len(raw) % 16)))

MEGA_FOLDER = "PUBFOLD1"                 # link: https://mega.nz/folder/PUBFOLD1#EBESExQVFhcYGRobHB0eHw
MEGA_MASTER = bytes(range(16, 32))
MEGA_FILES = [  # (handle, genitore, nome, dimensione)
    ("FILE0001", "ROOT0001", "L101_NSanityBeach.pak", 70000),
    ("FILE0002", "ROOT0001", "update.pak", 5000),
    ("FILE0003", "ROOT0001", "leggimi.txt", 10),
    ("FILE0004", "ROOT0001", "L102_Jungle.pak", 1234),
    ("FILE0005", "SUB00001", "L201_Sub.pak", 777),
]
MEGA_STATE = {"busy": 1}  # la prima richiesta di elenco risponde "occupato" (-3)

def mega_node_key(handle):
    return bytes((ord(handle[-1]) * 7 + j) & 0xFF for j in range(32))

def mega_file_key(nk):
    return bytes(nk[i] ^ nk[i + 16] for i in range(16))

def mega_nodes():
    folder_key = bytes(range(50, 66))
    nodes = [{"h": "ROOT0001", "p": "OWNER001", "t": 1, "u": "U1",
              "a": attrs("Livelli", folder_key), "k": "ROOT0001:" + b64(ecb(MEGA_MASTER, folder_key))},
             {"h": "SUB00001", "p": "ROOT0001", "t": 1, "u": "U1",
              "a": attrs("sottocartella", folder_key), "k": "ROOT0001:" + b64(ecb(MEGA_MASTER, folder_key))}]
    for h, parent, name, size in MEGA_FILES:
        nk = mega_node_key(h)
        nodes.append({"h": h, "p": parent, "t": 0, "u": "U1", "s": size, "ts": 1700000000,
                      "a": attrs(name, mega_file_key(nk)), "k": "XYZ00001:AAAA/ROOT0001:" + b64(ecb(MEGA_MASTER, nk))})
    return nodes

def mega_content(h):
    for fh, _, name, size in MEGA_FILES:
        if fh == h:
            nk = mega_node_key(h)
            return ctr(mega_file_key(nk), nk[16:24] + bytes(8), pak_bytes("mega-" + name, size))
    return None

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
        if u.path.startswith("/megadl/"):
            data = mega_content(u.path.rsplit("/", 1)[1])
            if data is None:
                return self.send_bytes(404, b"missing")
            return self.send_bytes(200, data)
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

    def do_POST(self):
        u = urlparse(self.path)
        qs = parse_qs(u.query)
        body = self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))
        if u.path != "/mega/cs":
            return self.send_bytes(404, b"no")
        try:
            cmd = json.loads(body)[0]
        except Exception:
            return self.send_bytes(200, b"-2", "application/json")
        if qs.get("n", [""])[0] != MEGA_FOLDER:
            return self.send_bytes(200, b"[-9]", "application/json")
        if cmd.get("a") == "f":
            if MEGA_STATE["busy"] > 0:
                MEGA_STATE["busy"] -= 1
                return self.send_bytes(200, b"-3", "application/json")
            return self.send_bytes(200, json.dumps([{"f": mega_nodes(), "ok": [], "s": []}]).encode(), "application/json")
        if cmd.get("a") == "g":
            for h, _, name, size in MEGA_FILES:
                if h == cmd.get("n"):
                    resp = {"s": size, "at": attrs(name, mega_file_key(mega_node_key(h))),
                            "g": "http://127.0.0.1:%d/megadl/%s" % (PORT, h)}
                    return self.send_bytes(200, json.dumps([resp]).encode(), "application/json")
            return self.send_bytes(200, b"[-9]", "application/json")
        return self.send_bytes(200, b"[-2]", "application/json")

if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "files"), exist_ok=True)
    files = {"L101_NSanityBeach.pak": 70000, "update.pak": 3000, "Nome con spazi.pak": 2048}
    for n, s in files.items():
        with open(os.path.join(ROOT, "files", n), "wb") as f:
            f.write(pak_bytes("http-" + n, s))
    with open(os.path.join(ROOT, "files", "Custom_Level.pak"), "wb") as f:
        f.write(level_pak("Crash1", "Custom_Level"))
    # livello nuovo convertito con --nuovo (registrazione in update/), in una cartella a parte
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import crea_pak_prova
    os.makedirs(os.path.join(ROOT, "nuovo"), exist_ok=True)
    crea_pak_prova.write_pak(os.path.join(ROOT, "nuovo", "Nuovo.pak"), crea_pak_prova.level_files("Nuovo"))
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
