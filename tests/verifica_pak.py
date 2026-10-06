#!/usr/bin/env python3
"""Controlla l'update.pak creato da Manager::registerLevel (archivio scritto da pak.cpp).

Rilegge l'archivio con il lettore di tools/controlla_pak.py, decomprime tutti i file e li
confronta con quelli attesi (tests/crea_pak_prova.py); controlla anche ordine degli hash e
parametri di ricerca (ogni file deve essere trovabile come fa il gioco).

Uso: python verifica_pak.py <update.pak> [base]
"""
import lzma
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from controlla_pak import Pak, fnv1a  # noqa: E402
from crea_pak_prova import BASE_FILES, expected_merged, ROOT  # noqa: E402


def read_entry(pak, i):
    comp, usize, base = pak.comp(i), pak.infos[i][2], pak.base(i)
    d = pak.d
    if comp == 0:
        return bytes(d[base:base + usize])
    out = bytearray()
    for boff, compressed, bsize in pak.blocks(i):
        p = base + boff
        if not compressed:
            out += d[p:p + bsize]
        elif comp == 2:
            cs = struct.unpack_from("<I", d, p)[0]
            props = d[p + 4]
            lc, rest = props % 9, props // 9
            lp, pb = rest % 5, rest // 5
            dict_size = struct.unpack_from("<I", d, p + 5)[0]
            filters = [{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}]
            dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
            out += dec.decompress(bytes(d[p + 9:p + 9 + cs]))[:bsize]
        elif comp == 1:
            cs = struct.unpack_from("<H", d, p)[0]
            out += zlib.decompress(bytes(d[p + 2:p + 2 + cs]), -15)[:bsize]
        else:
            raise ValueError("compressione %d" % comp)
    return bytes(out)


def main():
    pak = Pak(sys.argv[1])
    errors = []
    # "base": archivio riscritto senza modifiche
    expected = {p: d for p, d, _, _ in BASE_FILES} if sys.argv[2:] == ["base"] else expected_merged()
    if pak.version != 12:
        errors.append("versione %d" % pak.version)
    found = {}
    for i, (full, path) in enumerate(pak.paths):
        if full != ROOT + path:
            errors.append("percorso completo sbagliato: %s" % full)
        if pak.ids[i] != fnv1a(path):
            errors.append("hash sbagliato: %s" % path)
        found[path] = read_entry(pak, i)
    if list(pak.ids) != sorted(pak.ids):
        errors.append("hash non ordinati")
    if set(found) != set(expected):
        errors.append("file diversi: in piu' %s, mancanti %s" % (sorted(set(found) - set(expected)),
                                                                sorted(set(expected) - set(found))))
    for path, data in expected.items():
        if path in found and found[path] != data:
            errors.append("contenuto diverso: %s (%d byte invece di %d)" % (path, len(found[path]), len(data)))
    divider, margin, n = pak.header[5], pak.header[6], pak.n
    for h in pak.ids:
        q = h // divider
        lo, hi, ok = max(0, q - margin), min(n - 1, q + margin + 1), False
        while lo <= hi and not ok:
            mid = (lo + hi) // 2
            ok = pak.ids[mid] == h
            if pak.ids[mid] < h:
                lo = mid + 1
            else:
                hi = mid - 1
        if not ok:
            errors.append("hash 0x%08X non trovabile con divisore %d e margine %d" % (h, divider, margin))
    if errors:
        print("update.pak NON valido:\n  " + "\n  ".join(errors))
        sys.exit(1)
    print("update.pak valido: %d file, tutti con il contenuto atteso" % n)


if __name__ == "__main__":
    main()
