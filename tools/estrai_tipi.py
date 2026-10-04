#!/usr/bin/env python3
"""
estrai_tipi.py - elenca i tipi di oggetto (con la loro dimensione) usati dai file del gioco

Serve a capire come cambia la struttura degli oggetti tra versione PC e Switch.
Legge solo l'inizio di ogni file .igz (l'elenco dei tipi) e l'intestazione dei
file Havok (.hkx): non estrae e non copia dati del gioco.

Uso:
    python estrai_tipi.py --switch "<cartella dump Switch>" --pc "<cartella archives del gioco PC>"

    (--pc e' facoltativo; per la versione Steam di solito e'
     C:\\Program Files (x86)\\Steam\\steamapps\\common\\Crash Bandicoot N. Sane Trilogy\\archives)

Scrive tipi.json (qualche centinaio di KB): allegalo in chat.
"""

import argparse
import json
import os
import struct
import sys
import time
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from controlla_pak import Pak, parse_igz  # noqa: E402


def igz_prefix(pak, i):
    """Legge solo la parte dell'igz che contiene intestazione ed elenco dei tipi."""
    raw = pak.read(i, limit=0x1000)
    if len(raw) < 0x24:
        return raw
    off, need = 0x14, 0
    for _ in range(64):
        if off + 16 > len(raw):
            break
        _ident, coff, csize, _align = struct.unpack_from("<4i", raw, off)
        if coff == 0:
            break
        need = coff + csize  # il primo blocco e' quello dei fixup
        break
    if need > len(raw):
        raw = pak.read(i, limit=need)
    return raw


def scan(folder, label):
    paks = []
    for root, _, files in os.walk(folder):
        paks += [os.path.join(root, f) for f in files if f.lower().endswith(".pak")]
    if not paks:
        sys.exit("Nessun .pak in %s" % folder)
    seen = set()
    types = defaultdict(Counter)      # tipo -> {dimensione: numero di file}
    pools = Counter()
    plat = Counter()
    hkx = Counter()
    versions, roots = Counter(), Counter()
    errors = Counter()
    n_igz = 0
    t0 = time.time()
    for k, path in enumerate(sorted(paks)):
        try:
            pak = Pak(path)
        except Exception as e:
            errors["archivio: " + str(e)[:50]] += 1
            continue
        versions[pak.version] += 1
        for i, (full, short) in enumerate(pak.paths):
            low = short.lower()
            if low in seen:
                continue
            seen.add(low)
            if full.endswith(short) and len(full) > len(short):
                roots[full[:-len(short)]] += 1
            try:
                if low.endswith(".igz"):
                    info = parse_igz(igz_prefix(pak, i))
                    if not info:
                        errors["igz con firma sconosciuta"] += 1
                        continue
                    n_igz += 1
                    plat[info["piattaforma"]] += 1
                    pools.update(info["pool"])
                    for t, s in info["tipi"].items():
                        types[t][s] += 1
                elif low.endswith(".hkx"):
                    h = pak.read(i, limit=0x40)[:0x40]
                    if h[:8] == b"\x57\xE0\xE0\x57\x10\xC0\xC0\x10":
                        ver = h[0x28:0x38].split(b"\0")[0].decode("ascii", "replace")
                        hkx["%s regole %s" % (ver, h[16:20].hex())] += 1
                    else:
                        hkx["altro " + h[:8].hex()] += 1
            except Exception as e:
                errors[type(e).__name__ + ": " + str(e)[:50]] += 1
        pak.close()
        print("\r%s: %d/%d archivi, %d igz, %ds" % (label, k + 1, len(paks), n_igz, time.time() - t0),
              end="", file=sys.stderr)
    print(file=sys.stderr)
    return {
        "archivi": len(paks),
        "versione_archivio": dict(versions),
        "radici": dict(roots.most_common(3)),
        "igz": n_igz,
        "igz_piattaforma": dict(plat),
        "pool": dict(pools.most_common()),
        "hkx": dict(hkx.most_common(20)),
        "errori": dict(errors.most_common(10)),
        "tipi": {t: {str(s): c for s, c in sizes.items()} for t, sizes in sorted(types.items())},
    }


def main():
    ap = argparse.ArgumentParser(description="Elenca i tipi di oggetto dei file del gioco")
    ap.add_argument("--switch", required=True, help="cartella del dump RomFS della Switch")
    ap.add_argument("--pc", help="cartella archives del gioco PC (facoltativo)")
    ap.add_argument("-o", "--output", default="tipi.json")
    a = ap.parse_args()

    out = {"versione_script": 1, "switch": scan(a.switch, "Switch")}
    if a.pc:
        out["pc"] = scan(a.pc, "PC")
    with open(a.output, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=0, ensure_ascii=False)
    s = out["switch"]
    print("Switch: %d archivi, %d igz, %d tipi" % (s["archivi"], s["igz"], len(s["tipi"])))
    if "pc" in out:
        p = out["pc"]
        print("PC:     %d archivi, %d igz, %d tipi" % (p["archivi"], p["igz"], len(p["tipi"])))
    print("Scritto %s: allegalo in chat." % a.output)


if __name__ == "__main__":
    main()
