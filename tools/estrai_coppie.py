#!/usr/bin/env python3
"""
estrai_coppie.py - prepara coppie di file uguali (versione PC e versione Switch) per mettere a punto il convertitore

Dal .pak fatto sul PC prende i file .igz di gioco (non grafica) che esistono anche negli
originali Switch, sceglie un insieme piccolo che copre piu' tipi di oggetto possibile e li
salva decompressi in uno zip, insieme ai file del livello che andranno convertiti.

Uso:
    python estrai_coppie.py MIO.pak --switch "<cartella del dump Switch>" [-o coppie.zip] [--mb 8]

Contenuto dello zip:
    pc/<percorso>       file presi dal .pak PC
    switch/<percorso>   lo stesso file preso dagli originali Switch
    livello/<percorso>  i file propri del livello (solo PC: sono quelli da convertire)
    indice.json         elenco delle coppie e dei tipi che contengono
"""

import argparse
import json
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from controlla_pak import Pak, parse_igz, level_names  # noqa: E402

# tipi grafici: si prendono dagli originali Switch, non vanno convertiti
GRAPHIC = re.compile(r"^(igImage2|igGraphics\w*|ig\w*VertexBuffer|ig\w*IndexBuffer|igModel\w*|igShader\w*|"
                     r"\w*Material\w*|igTexture\w*|CMaterialHandleTableInfo|igVertexFormat\w*|CGraphicsSkinInfo|"
                     r"igSkeleton\w*|igAnimation\w*|CSoundSample)$")


def main():
    ap = argparse.ArgumentParser(description="Coppie PC/Switch per il convertitore")
    ap.add_argument("mio")
    ap.add_argument("--switch", required=True)
    ap.add_argument("-o", "--output", default="coppie.zip")
    ap.add_argument("--mb", type=float, default=8, help="dimensione massima delle coppie (MB)")
    ap.add_argument("--mb-livello", type=float, default=12, help="dimensione massima dei file del livello (MB)")
    a = ap.parse_args()

    mine = Pak(a.mio)
    paths = []
    for root, _, files in os.walk(a.switch):
        paths += [os.path.join(root, f) for f in files if f.lower().endswith(".pak")]
    if os.path.isfile(a.switch):
        paths = [a.switch]
    print("Indicizzo %d archivi Switch..." % len(paths), file=sys.stderr)
    index, sw = {}, {}
    for p in sorted(paths):
        try:
            pk = Pak(p)
        except Exception:
            continue
        sw[p] = pk
        for i, (_, short) in enumerate(pk.paths):
            index.setdefault(short.lower(), (p, i))

    levels = [lvl for _, lvl in level_names(mine.paths)]
    level = levels[0] if levels else None

    candidates, level_files = [], []
    for i, (_, short) in enumerate(mine.paths):
        low = short.lower()
        if not low.endswith(".igz"):
            continue
        # i file del livello (mappe, registrazione, pacchetto) sono quelli modificati dall'editor
        own = low.startswith(("maps/", "update/", "packages/")) or (level and level.lower() in low)
        if own:
            level_files.append(i)
            continue
        cp = index.get(low)
        if not cp:
            continue
        size = mine.infos[i][2]
        if size > 400_000:
            continue
        try:
            info = parse_igz(mine.read(i, limit=0x1000) if size > 0x1000 else mine.read(i))
        except Exception:
            continue
        if not info:
            continue
        types = [t for t in info["tipi"] if not GRAPHIC.match(t)]
        if not types or len(types) < len(info["tipi"]) * 0.5:
            continue  # file soprattutto grafici
        candidates.append((i, cp, size, set(types)))

    # scelta: piu' tipi nuovi per byte, fino al limite
    budget = int(a.mb * 1024 * 1024)
    covered, chosen, used = set(), [], 0
    while candidates:
        best = max(candidates, key=lambda c: len(c[3] - covered) / (c[2] + 20_000))
        if not (best[3] - covered) or used + best[2] > budget:
            break
        chosen.append(best)
        covered |= best[3]
        used += best[2]
        candidates.remove(best)

    entries = []
    with zipfile.ZipFile(a.output, "w", zipfile.ZIP_DEFLATED) as z:
        for i, (sp, si), size, types in chosen:
            short = mine.paths[i][1]
            z.writestr("pc/" + short, mine.read(i))
            z.writestr("switch/" + short, sw[sp].read(si))
            entries.append({"percorso": short, "tipi": sorted(types)})
        lbudget, lused, lnames = int(a.mb_livello * 1024 * 1024), 0, []
        for i in sorted(level_files, key=lambda k: mine.infos[k][2]):
            size = mine.infos[i][2]
            if lused + size > lbudget:
                continue
            z.writestr("livello/" + mine.paths[i][1], mine.read(i))
            lnames.append(mine.paths[i][1])
            lused += size
        z.writestr("indice.json", json.dumps({
            "pak": mine.name, "livello": level, "coppie": entries, "file_livello": lnames,
            "file_livello_esclusi": len(level_files) - len(lnames), "tipi_coperti": len(covered),
        }, indent=1))
    print("Coppie: %d file (%.1f MB, %d tipi); file del livello: %d (%.1f MB)" % (
        len(chosen), used / 1048576, len(covered), len(lnames), lused / 1048576))
    print("Scritto %s: allegalo in chat." % a.output)


if __name__ == "__main__":
    main()
