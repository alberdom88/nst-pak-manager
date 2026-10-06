#!/usr/bin/env python3
"""
list_originals.py - crea originali.txt, l'elenco dei .pak originali del gioco

Facoltativo: l'app sceglie da sola il nome di installazione (per un livello quello
che ha dentro). L'elenco serve per la scelta a mano (tasto B) e per scrivere i nomi
con le maiuscole giuste. Prendi l'elenco dal dump della RomFS (in Eden: tasto destro
sul gioco -> Dump RomFS) e copia il file creato in sdmc:/switch/nst-pak-manager/.

Uso:
    python list_originals.py <cartella_dump> [altre_cartelle...] [-o originali.txt]

Se indichi piu' cartelle (es. gioco base e aggiornamento) gli elenchi vengono uniti.
"""
import argparse
import os
import sys


def collect(folder):
    in_archives, anywhere = set(), set()
    for root, _, files in os.walk(folder):
        is_archives = os.path.basename(root).lower() == "archives"
        for name in files:
            if name.lower().endswith(".pak"):
                anywhere.add(name)
                if is_archives:
                    in_archives.add(name)
    # Atmosphere sostituisce i file di romfs/archives: se c'e' quella cartella, basta lei
    return in_archives or anywhere


def main():
    ap = argparse.ArgumentParser(description="Crea originali.txt dal dump della RomFS")
    ap.add_argument("folders", nargs="+")
    ap.add_argument("-o", "--output", default="originali.txt")
    a = ap.parse_args()

    names = {}
    for folder in a.folders:
        if not os.path.isdir(folder):
            sys.exit(f"Cartella non trovata: {folder}")
        for n in collect(folder):
            names.setdefault(n.lower(), n)
    if not names:
        sys.exit("Nessun file .pak trovato: indica la cartella del dump RomFS.")

    ordered = sorted(names.values(), key=str.lower)
    with open(a.output, "w", encoding="utf-8", newline="\n") as f:
        f.write("# .pak originali di Crash Bandicoot N. Sane Trilogy\n")
        f.write("\n".join(ordered) + "\n")
    print(f"{len(ordered)} file scritti in {a.output}")


if __name__ == "__main__":
    main()
