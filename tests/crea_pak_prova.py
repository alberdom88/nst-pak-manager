#!/usr/bin/env python3
"""Archivi .pak di prova (versione 12, Switch) per i test di pak.cpp e Manager::registerLevel.

  base_update.pak    come l'update.pak originale: file compressi LZMA (piccoli e medi, un blocco
                     lasciato non compresso), ZLIB e non compressi; tabelle dei blocchi SENZA la
                     voce finale (per non dipendere da come le scrive lo strumento originale)
  Custom_Level.pak   livello nuovo con i file di registrazione in update/ (non compressi)

Uso: python crea_pak_prova.py <cartella>
"""
import lzma
import os
import struct
import sys
import zlib

ROOT = "temporary/mack/data/nx/output/"
SECTOR = 0x800
SMALL, MEDIUM = 0x7F, 0x7FFF
PKG = "packages/generated/chunkinfos/chunkinfos_pkg.igz"


def fnv1a(s):
    h = 0x811C9DC5
    for b in s.lower().replace("\\", "/").encode("utf-8"):
        h = ((h ^ b) * 0x1000193) & 0xFFFFFFFF
    return h


def content(tag, size):
    """Dati prevedibili ma poco ripetitivi (cosi' la compressione ha senso)"""
    out = bytearray()
    x = fnv1a(tag)
    while len(out) < size:
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out += (tag + ":%08x;" % (x >> 8)).encode()
    return bytes(out[:size])


def noise(size, seed):
    out = bytearray()
    x = seed
    while len(out) < size:
        x = (x * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        out += struct.pack("<Q", x)
    return bytes(out[:size])


LZMA_FILTERS = [{"id": lzma.FILTER_LZMA1, "dict_size": 0x8000, "lc": 3, "lp": 0, "pb": 2}]
LZMA_PROPS = bytes([(2 * 5 + 0) * 9 + 3]) + struct.pack("<I", 0x8000)


def lzma_block(raw):
    payload = lzma.compress(raw, format=lzma.FORMAT_RAW, filters=LZMA_FILTERS)
    return struct.pack("<I", len(payload)) + LZMA_PROPS + payload


def zlib_block(raw):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    payload = c.compress(raw) + c.flush()
    return struct.pack("<H", len(payload)) + payload


def size_class(size):
    if size <= SMALL * SECTOR:
        return 0, SMALL, 0x80
    if size <= MEDIUM * SECTOR:
        return 1, MEDIUM, 0x8000
    return 2, 0x7FFFFFFF, 0x80000000


def stored(data, compression, raw_blocks=()):
    """Dati come stanno nell'archivio e voci della tabella dei blocchi (senza la voce finale)"""
    if compression == 0:
        return data, []
    _, _, flag = size_class(len(data))
    out, entries = bytearray(), []
    for k in range(0, len(data), 0x8000):
        chunk = data[k:k + 0x8000]
        if (k // 0x8000) in raw_blocks:
            block, compressed = chunk, False
        else:
            block, compressed = (lzma_block(chunk) if compression == 2 else zlib_block(chunk)), True
        entries.append((len(out) // SECTOR) | (flag if compressed else 0))
        out += block
        out += b"\0" * (-len(out) % SECTOR)
    # l'ultimo blocco senza riempimento: i dati del file finiscono li'
    last_len = len(block)
    return bytes(out[:len(out) - (-last_len % SECTOR)]), entries


def write_pak(path, files):
    """files: lista di (percorso, dati, compressione, blocchi_non_compressi)"""
    items = []
    for p, data, comp, raw_blocks in files:
        blob, entries = stored(data, comp, raw_blocks)
        items.append({"path": p, "size": len(data), "comp": comp, "blob": blob, "entries": entries, "hash": fnv1a(p)})
    items.sort(key=lambda it: it["hash"])
    n = len(items)
    tables = ([], [], [])
    for it in items:
        if it["comp"] == 0:
            it["bi"] = 0xFFFFFFFF
            continue
        cls = size_class(it["size"])[0]
        it["bi"] = (it["comp"] << 28) | len(tables[cls])
        tables[cls].extend(it["entries"])  # niente voce finale
    divider = 0xFFFFFFFF // n
    hashes = [it["hash"] for it in items]

    def findable(margin, target):
        q = target // divider
        lo, hi = max(0, q - margin), min(n - 1, q + margin + 1)
        while lo <= hi:
            mid = (lo + hi) // 2
            if hashes[mid] == target:
                return True
            if hashes[mid] < target:
                lo = mid + 1
            else:
                hi = mid - 1
        return False

    margin = next(m for m in range(n) if all(findable(m, h) for h in hashes))
    toc = 20 * n + 4 * len(tables[2]) + 2 * len(tables[1]) + len(tables[0])
    files_off = (0x38 + toc + SECTOR - 1) // SECTOR * SECTOR
    pos, body = files_off, bytearray()
    for it in items:
        it["off"] = pos
        padded = it["blob"] + b"\0" * (-len(it["blob"]) % SECTOR)
        body += padded
        pos += len(padded)
    paths_start = pos
    names = bytearray()
    rel = []
    for it in items:
        rel.append(4 * n + len(names))
        names += (ROOT + it["path"]).encode() + b"\0" + it["path"].encode() + b"\0" + b"\0" * 4
    table = struct.pack("<%dI" % n, *rel) + names
    head = struct.pack("<10IQ2I", 0x1A414749, 12, toc, n, SECTOR, divider, margin,
                       len(tables[2]), len(tables[1]), len(tables[0]), paths_start, len(table), 1)
    head += struct.pack("<%dI" % n, *hashes)
    for i, it in enumerate(items):
        head += struct.pack("<IiiI", it["off"], (i - 1) * 256, it["size"], it["bi"])
    head += struct.pack("<%dI" % len(tables[2]), *tables[2])
    head += struct.pack("<%dH" % len(tables[1]), *tables[1])
    head += bytes(tables[0])
    head += b"\0" * (files_off - len(head))
    with open(path, "wb") as f:
        f.write(head + body + table)


BASE_FILES = [
    ("maps/crash1/l101_nsanitybeach/l101_nsanitybeach_zoneinfo.igz", content("zone101", 5000), 2, ()),
    (PKG, content("pkg-originale", 300000), 2, ()),
    ("scripts/leggimi.lua", content("lua", 3000), 0, ()),
    ("misc/rumore.bin", content("misto", 0x8000) + noise(0x8000, 7) + content("coda", 20000), 2, (1,)),
    ("misc/zlib.igz", content("zlib", 40000), 1, ()),
    ("misc/grande_medio.bin", content("medio", 70000), 2, ()),
]

def level_files(name):
    """Livello nuovo convertito con --nuovo: file del livello e registrazione in update/"""
    low = name.lower()
    return [
        ("packages/generated/maps/crash1/%s/%s_pkg.igz" % (low, name), content("livello-pkg", 4000), 0, ()),
        ("maps/crash1/%s/%s.igz" % (low, low), content("livello", 90000), 2, ()),
        ("update/maps/crash1/%s/%s_zoneinfo.igz" % (low, low), content("zone-nuova", 2600), 0, ()),
        ("update/" + PKG, content("pkg-con-registrazione", 310000), 0, ()),
    ]


LEVEL_FILES = level_files("custom_level")


def expected_merged():
    """Contenuto atteso di update.pak dopo l'unione"""
    out = {p: d for p, d, _, _ in BASE_FILES}
    for p, d, _, _ in LEVEL_FILES:
        if p.startswith("update/"):
            out[p[7:]] = d
    return out


if __name__ == "__main__":
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    write_pak(os.path.join(folder, "base_update.pak"), BASE_FILES)
    write_pak(os.path.join(folder, "Custom_Level.pak"), LEVEL_FILES)
