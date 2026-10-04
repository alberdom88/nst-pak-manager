#!/usr/bin/env python3
"""
cerca_opzioni.py - cerca nell'eseguibile Switch del gioco le opzioni di avvio

Il gioco PC accetta "-om <livello>" sulla riga di comando. Questo script legge
l'eseguibile della versione Switch (il file "main" della ExeFS, formato NSO) ed
elenca solo i TESTI che riguardano argomenti, opzioni e file di configurazione:
serve a capire se l'opzione esiste con un altro nome o in un'altra forma
(per esempio un file di testo letto all'avvio). Non copia il codice del gioco.

Come ottenere il file "main":
  in Eden attiva "Dump ExeFS" (o "Dump Decompressed NSOs") nelle impostazioni
  (Configura -> Sistema -> File system, oppure Configura -> Debug),
  avvia il gioco una volta e chiudilo: i file compaiono in
  %APPDATA%\\eden\\dump\\0100D1B006744000\\

Uso:
  python cerca_opzioni.py "<file main oppure cartella del dump>"

Scrive opzioni.txt (pochi KB): allegalo in chat.
"""

import os
import re
import struct
import sys

try:
    import lz4.block as _lz4  # facoltativo: piu' veloce (pip install lz4)
except Exception:  # noqa: BLE001
    _lz4 = None


def lz4_block_decompress(src, size):
    """Decompressione di un blocco LZ4 (formato a blocchi, senza intestazione)"""
    if _lz4 is not None:
        return _lz4.decompress(src, uncompressed_size=size)
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(dst) - off
        if off <= 0 or start < 0:
            raise ValueError("blocco LZ4 non valido")
        if off >= ml:
            dst += dst[start:start + ml]
        else:
            for k in range(ml):
                dst.append(dst[start + k])
    if len(dst) != size:
        raise ValueError("dimensione decompressa %d invece di %d" % (len(dst), size))
    return bytes(dst)


def read_nso(data):
    """Segmenti (text, rodata, data) decompressi e ID della build"""
    if data[:4] != b"NSO0":
        raise ValueError("non e' un file NSO")
    flags = struct.unpack_from("<I", data, 0x0C)[0]
    build_id = data[0x40:0x60].hex().upper()
    segments = []
    for k in range(3):
        file_off, mem_off, size = struct.unpack_from("<3I", data, 0x10 + 0x10 * k)
        file_size = struct.unpack_from("<I", data, 0x60 + 4 * k)[0]
        raw = data[file_off:file_off + file_size]
        if flags & (1 << k) and len(raw) != size:  # i dump "decompressi" restano col flag acceso
            raw = lz4_block_decompress(raw, size)
        segments.append(raw)
    return segments, build_id


STRING = re.compile(rb"[\x20-\x7e]{3,200}")

GROUPS = [
    ("Argomenti della riga di comando", re.compile(
        r"gethostarg|argc|argv|command.?line|cmd.?line|commandline|\bargs?\b", re.I)),
    ("Apertura diretta di un livello", re.compile(
        r"open.?map|^-?om$|^-om\b|start.?map|boot.?map|start.?zone|boot.?zone|load.?map|level.?select|"
        r"startup.?(map|zone|level)|first.?(map|zone)|initial.?(map|zone)|skip.?(intro|menu|title|front)", re.I)),
    ("Opzioni (testi che iniziano con - o +)", re.compile(r"^[-+][A-Za-z][A-Za-z0-9_\-]{0,40}$")),
    ("File di configurazione e percorsi", re.compile(
        r"\.(txt|ini|cfg|config|xml|json)\b|^(rom|host|sd|sdmc|save|cache|data|archive)s?:/|commandline", re.I)),
    ("Debug e sviluppo", re.compile(
        r"debug.?(menu|map|level|zone|start|option)|dev.?(menu|mode)|cheat|console.?(var|command)|\bcvar", re.I)),
]

MAX_PER_GROUP = 300


def strings_of(segment):
    for m in STRING.finditer(segment):
        yield m.group().decode("ascii", "replace").strip()


def find_inputs(path):
    if os.path.isfile(path):
        return [path]
    found = []
    for root, _, files in os.walk(path):
        for name in files:
            full = os.path.join(root, name)
            try:
                with open(full, "rb") as f:
                    if f.read(4) == b"NSO0":
                        found.append(full)
            except OSError:
                pass
    # "main" per primo: e' l'eseguibile del gioco
    found.sort(key=lambda p: (os.path.basename(p).lower() not in ("main", "main.nso"), p))
    return found


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    inputs = find_inputs(sys.argv[1])
    if not inputs:
        sys.exit("Nessun file NSO trovato in %s" % sys.argv[1])

    out = ["RICERCA OPZIONI DI AVVIO (solo testi)", ""]
    for path in inputs:
        with open(path, "rb") as f:
            data = f.read()
        try:
            segments, build_id = read_nso(data)
        except Exception as e:  # noqa: BLE001
            out.append("%s: non letto (%s)" % (path, e))
            continue
        print("Leggo %s..." % os.path.basename(path), file=sys.stderr)

        texts = []
        seen = set()
        for segment in segments[1:]:  # rodata e data: li' stanno i testi
            for s in strings_of(segment):
                if s not in seen:
                    seen.add(s)
                    texts.append(s)

        out.append("=== %s ===" % os.path.basename(path))
        out.append("ID build: %s" % build_id)
        out.append("dimensioni: text %d, rodata %d, data %d; testi %d" % (
            len(segments[0]), len(segments[1]), len(segments[2]), len(texts)))
        out.append("legge gli argomenti di avvio (nn::os::GetHostArgc/Argv): %s" % (
            "SI" if any("GetHostArg" in s for s in texts) else "no"))
        for title, pattern in GROUPS:
            hits = [s for s in texts if pattern.search(s)]
            out.append("")
            out.append("--- %s: %d ---" % (title, len(hits)))
            for s in hits[:MAX_PER_GROUP]:
                out.append("  " + s)
            if len(hits) > MAX_PER_GROUP:
                out.append("  ... altri %d" % (len(hits) - MAX_PER_GROUP))
        out.append("")

    with open("opzioni.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("Scritto opzioni.txt: allegalo in chat.")


if __name__ == "__main__":
    main()
