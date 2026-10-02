#!/usr/bin/env python3
"""
make_manifest.py - crea manifest.json per una cartella di .pak

Serve quando i file stanno su un hosting che non mostra l'elenco della
cartella (NAS, hosting statico, ecc.). Metti manifest.json nella stessa
cartella dei .pak e nella config dell'app usa l'URL del manifest:

    {"name": "Hosting", "type": "http", "url": "https://esempio.it/pak/manifest.json"}

Uso: python make_manifest.py <cartella_con_i_pak>
"""
import json
import os
import sys

folder = sys.argv[1] if len(sys.argv) > 1 else "."
files = []
for name in sorted(os.listdir(folder), key=str.lower):
    path = os.path.join(folder, name)
    if name.lower().endswith(".pak") and os.path.isfile(path):
        files.append({"name": name, "size": os.path.getsize(path)})

out = os.path.join(folder, "manifest.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump({"files": files}, f, indent=2, ensure_ascii=False)
print(f"{len(files)} file scritti in {out}")
