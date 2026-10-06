#!/usr/bin/env python3
r"""
crea_avvio_livello.py - fa partire il gioco direttamente dentro un livello, senza argomenti di avvio

Il gioco all'avvio legge il file di configurazione di sviluppo "debug.xml" e, se contiene
    <MAP filename="crash1/l112_roadtonowhere/l112_roadtonowhere"/>
carica quel livello invece del menu (e' la stessa cosa che fa l'opzione -om). Nella versione
in commercio pero' la lettura e' spenta: la funzione CConfigSystem::InFinal risponde "no".
Questo script crea:
  - una patch IPS che cambia quella sola risposta in "si'" (un'istruzione, 4 byte),
    per l'eseguibile esatto del tuo dump (il nome del file e' l'ID della build);
  - il file debug.xml con il livello da aprire.
Il gioco originale non viene modificato: patch e file si mettono nelle cartelle delle mod.

Uso:
  python crea_avvio_livello.py "<file main o cartella del dump ExeFS>" [<livello>] [cartella_uscita]
                               [--romfs "<cartella del dump RomFS>"]
    <livello>: facoltativo; un .pak con un livello (il nome viene letto dal file) oppure il
               nome completo, per esempio crash1/l112_roadtonowhere/l112_roadtonowhere.
               Serve per Eden (che non ha l'app); sulla Switch debug.xml lo scrive NST Pak Manager.
    La cartella di Eden con i dump del gioco va bene sia come ExeFS sia come RomFS:
    %APPDATA%\eden\dump\0100D1B006744000

Crea (in cartella_uscita, predefinita "avvio_livello"):
  eden/NST avvio livello/exefs/<ID build>.ips      -> cartella delle mod di Eden per il gioco
  eden/NST avvio livello/romfs/debug.xml           (solo con <livello>)
  sd/atmosphere/exefs_patches/nst_avvio_livello/<ID build>.ips   -> radice della SD
  sd/atmosphere/contents/0100D1B006744000/romfs/debug.xml        (solo con <livello>)

Con --romfs copia anche l'update.pak originale del gioco in
  sd/switch/nst-pak-manager/originali/update.pak
che NST Pak Manager usa per registrare i livelli nuovi (convertiti con --nuovo).

Per tornare all'avvio normale basta togliere debug.xml (la patch da sola non cambia nulla).
"""

import os
import re
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analizza_avvio import Module  # noqa: E402

TITLE_ID = "0100D1B006744000"
SYMBOL = "_ZN13CConfigSystem7InFinalEv"
MOV_W0_ZERO = 0x2A1F03E0  # mov w0, wzr   (risponde "no")
MOV_W0_ONE = 0x52800020   # mov w0, #1    (risponde "si'")
RET = 0xD65F03C0
NSO_HEADER = 0x100        # le patch IPS contano anche l'intestazione dell'NSO


def find_main(path):
    if os.path.isfile(path):
        return path
    for root, _, files in os.walk(path):
        for name in files:
            if name.lower() in ("main", "main.nso"):
                return os.path.join(root, name)
    sys.exit("File main non trovato in %s" % path)


def levels_in_pak(path):
    """Identificativi dei livelli contenuti in un .pak (packages/generated/maps/<gioco>/<L>/<L>_pkg.igz)"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x38 or struct.unpack_from("<I", data, 0)[0] != 0x1A414749:
        sys.exit("%s non e' un archivio .pak" % path)
    count = struct.unpack_from("<I", data, 0x0C)[0]
    table_off = struct.unpack_from("<Q", data, 0x28)[0]
    table_size = struct.unpack_from("<I", data, 0x30)[0]
    table = data[table_off:table_off + table_size]
    out = []
    for i in range(count):
        rel = struct.unpack_from("<I", table, 4 * i)[0]
        a = table.find(b"\0", rel)
        b = table.find(b"\0", a + 1)
        short = table[a + 1:b].decode("utf-8", "replace")
        m = re.match(r"packages/generated/maps/([^/]+)/([^/]+)/([^/]+)_pkg\.igz$", short, re.I)
        if m:
            out.append(("%s/%s/%s" % m.groups()).lower())
    return out


def ips32(offset, payload):
    if offset >> 32 or offset == 0x45454F46:  # "EEOF" non puo' essere un offset
        raise ValueError("offset non valido")
    return b"IPS32" + struct.pack(">IH", offset, len(payload)) + payload + b"EEOF"


def debug_xml(level):
    return ('<?xml version="1.0" encoding="ascii"?>\n'
            "<config>\n"
            '\t<MAP filename="%s"/>\n'
            "</config>\n" % level)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print("  " + path)


def find_update_pak(romfs):
    for root, _, files in os.walk(romfs):
        for name in files:
            if name.lower() == "update.pak":
                return os.path.join(root, name)
    return None


def main():
    args = sys.argv[1:]
    romfs = None
    if "--romfs" in args:
        k = args.index("--romfs")
        if k + 1 >= len(args):
            sys.exit("--romfs vuole la cartella del dump RomFS")
        romfs = args[k + 1]
        del args[k:k + 2]
        if not os.path.isdir(romfs):
            sys.exit("Cartella del dump RomFS non trovata: %s" % romfs)
    if len(args) < 1:
        sys.exit(__doc__)
    main_path = find_main(args[0])
    level = args[1] if len(args) > 1 else None
    out_dir = args[2] if len(args) > 2 else "avvio_livello"
    if level and not level.lower().endswith(".pak") and "/" not in level and os.path.isdir(level):
        out_dir, level = level, None  # solo cartella di uscita, senza livello

    if level and level.lower().endswith(".pak"):
        found = levels_in_pak(level)
        if not found:
            sys.exit("%s non contiene un livello" % level)
        if len(found) > 1:
            print("Il .pak contiene piu' livelli, uso il primo: %s" % ", ".join(found))
        level = found[0]
    if level:
        level = level.strip().strip("/").lower()
    if level and not re.match(r"^[a-z0-9_]+/[a-z0-9_]+/[a-z0-9_]+$", level):
        sys.exit("Nome del livello non valido: %s (atteso: gioco/cartella/livello)" % level)

    with open(main_path, "rb") as f:
        raw = f.read()
    m = Module(raw)
    addr = next((v for v, n in m.func_at.items() if n == SYMBOL), None)
    if addr is None:
        sys.exit("Funzione %s non trovata: e' l'eseguibile giusto?" % SYMBOL)
    first, second = m.u32(addr), m.u32(addr + 4)
    if first == MOV_W0_ONE and second == RET:
        sys.exit("Questo eseguibile risulta gia' modificato (la funzione risponde gia' 'si'').")
    if first != MOV_W0_ZERO or second != RET:
        sys.exit("La funzione %s non e' quella attesa (0x%08X 0x%08X): versione del gioco diversa?"
                 % (SYMBOL, first, second))

    build_id = raw[0x40:0x60].hex().upper()
    patch = ips32(addr + NSO_HEADER, struct.pack("<I", MOV_W0_ONE))
    print("Eseguibile: %s (ID build %s)" % (main_path, build_id))
    print("Funzione %s a 0x%X: 'mov w0, wzr' -> 'mov w0, #1'" % (SYMBOL, addr))
    print("Livello: %s" % (level or "nessuno (debug.xml non creato: sulla Switch lo scrive l'app)"))
    print("File creati:")
    write(os.path.join(out_dir, "eden", "NST avvio livello", "exefs", build_id + ".ips"), patch)
    write(os.path.join(out_dir, "sd", "atmosphere", "exefs_patches", "nst_avvio_livello", build_id + ".ips"), patch)
    if level:
        xml = debug_xml(level).encode("ascii")
        write(os.path.join(out_dir, "eden", "NST avvio livello", "romfs", "debug.xml"), xml)
        write(os.path.join(out_dir, "sd", "atmosphere", "contents", TITLE_ID, "romfs", "debug.xml"), xml)
    if romfs:
        dest = os.path.join(out_dir, "sd", "switch", "nst-pak-manager", "originali", "update.pak")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        source = find_update_pak(romfs)
        if source:
            shutil.copyfile(source, dest)
            print("  %s (copia di %s)" % (dest, source))
        else:
            open(dest, "wb").close()
            print("  %s (vuoto: nel dump non c'e' update.pak)" % dest)


if __name__ == "__main__":
    main()
