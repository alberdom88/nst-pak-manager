#!/usr/bin/env python3
"""
controlla_pak.py - perche' un .pak non funziona sulla Switch, e quanto e' difficile convertirlo

Confronta il .pak da installare con gli originali della Switch (dump RomFS) e controlla:
  1. il NOME INTERNO del livello (rinominare il .pak non rinomina il contenuto);
  2. il CONTENITORE: versione dell'archivio, percorsi, compressione, identificativi;
  3. il CONTENUTO: per ogni file cerca lo stesso file negli originali Switch e li confronta
     byte per byte; per i file .igz confronta anche i tipi di oggetto e le loro dimensioni.

Uso:
    python controlla_pak.py MIO.pak --switch <cartella del dump o un .pak Switch> [--come NOME.pak]

Stampa un riassunto e scrive il rapporto completo in report_<MIO>.txt (allegalo in chat).
Il rapporto contiene solo nomi di file interni, tipi e parametri: nessun dato del gioco.
Solo libreria standard, non modifica nessun file.
"""

import argparse
import lzma
import mmap
import os
import re
import struct
import sys
import zlib
from collections import Counter, defaultdict

PAK_SIGNATURE = 0x1A414749
IGZ_SIGNATURE = 0x49475A01
HKX_MAGIC = b"\x57\xE0\xE0\x57\x10\xC0\xC0\x10"
SECTOR = 0x800
COMP = {0: "nessuna", 1: "ZLIB", 2: "LZMA", 3: "LZ4"}
SMALL, MEDIUM, LARGE = 0x7F, 0x7FFF, 0x7FFFFFFF
HEADER_FIELDS = ("firma", "versione", "dim. indice", "file", "settore", "hashSearchDivider",
                 "hashSearchMargin", "blocchi grandi", "blocchi medi", "blocchi piccoli",
                 "offset percorsi", "dim. percorsi", "flags")


def fnv1a(s):
    h = 0x811C9DC5
    for b in s.lower().replace("\\", "/").encode("utf-8"):
        h = ((h ^ b) * 0x1000193) & 0xFFFFFFFF
    return h


class Pak:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.f = open(path, "rb")
        try:
            self.d = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:
            raise ValueError("file vuoto")
        d = self.d
        if len(d) < 0x38:
            raise ValueError("file troppo piccolo")
        self.header = struct.unpack_from("<10IQ2I", d, 0)
        (sig, self.version, _toc, n, self.sector, _hd, _hm, nl, nm, ns, self.pt_off, _pts, self.flags) = self.header
        if sig != PAK_SIGNATURE:
            raise ValueError("non e' un .pak (firma 0x%08X)" % sig)
        self.n = n
        off = 0x38
        self.ids = struct.unpack_from("<%dI" % n, d, off)
        off += 4 * n
        self.infos = [struct.unpack_from("<IiiI", d, off + 16 * i) for i in range(n)]
        off += 16 * n
        self.large = struct.unpack_from("<%dI" % nl, d, off); off += 4 * nl
        self.medium = struct.unpack_from("<%dH" % nm, d, off); off += 2 * nm
        self.small = struct.unpack_from("<%dB" % ns, d, off)
        self.paths = []
        for o in struct.unpack_from("<%dI" % n, d, self.pt_off):
            p = self.pt_off + o
            e1 = d.find(b"\0", p)
            e2 = d.find(b"\0", e1 + 1)
            self.paths.append((d[p:e1].decode("utf-8", "replace"), d[e1 + 1:e2].decode("utf-8", "replace")))
        self.hdr = None

    def close(self):
        self.d.close()
        self.f.close()

    def comp(self, i):
        bi = self.infos[i][3]
        return 0 if bi == 0xFFFFFFFF else (bi >> 28) & 0xF

    def base(self, i):
        o, ordinal, _, _ = self.infos[i]
        return o if (ordinal & 1) == 0 else o + 0x100000000

    def blocks(self, i):
        usize, bi = self.infos[i][2], self.infos[i][3]
        if usize <= SMALL * SECTOR:
            t, mask, shift = self.small, SMALL, 7
        elif usize <= MEDIUM * SECTOR:
            t, mask, shift = self.medium, MEDIUM, 15
        else:
            t, mask, shift = self.large, LARGE, 31
        start = bi & 0xFFFFFFF
        out = []
        for k in range((usize + 0x7FFF) >> 15):
            b = t[start + k]
            size = (usize & 0x7FFF) if usize < (k + 1) * 0x8000 else 0x8000
            out.append(((b & mask) * SECTOR, (b >> shift) == 1, size))
        return out

    def lzma_header(self):
        if self.hdr is None:
            self.hdr = 2
            for i in range(self.n):
                if self.comp(i) != 2:
                    continue
                for boff, compressed, bsize in self.blocks(i):
                    if not compressed:
                        continue
                    for h in (2, 4):
                        try:
                            lzma_block(self.d, self.base(i) + boff, h, bsize)
                            self.hdr = h
                            return h
                        except Exception:
                            pass
                    self.hdr = 0
                    return 0
        return self.hdr

    def read(self, i, limit=None):
        c, usize, base, d = self.comp(i), self.infos[i][2], self.base(i), self.d
        if c == 0:
            return bytes(d[base:base + (min(usize, limit) if limit else usize)])
        hdr = self.lzma_header()
        out = bytearray()
        for boff, compressed, bsize in self.blocks(i):
            p = base + boff
            if not compressed:
                out += d[p:p + bsize]
            elif c == 2:
                out += lzma_block(d, p, hdr, bsize)
            elif c == 1:
                cs = struct.unpack_from("<H", d, p)[0]
                out += zlib.decompress(bytes(d[p + 2:p + 2 + cs]), -15)[:bsize]
            else:
                raise NotImplementedError("compressione " + COMP.get(c, str(c)))
            if limit and len(out) >= limit:
                break
        return bytes(out)


def lzma_block(d, p, hdr, usize):
    cs = struct.unpack_from("<H" if hdr == 2 else "<I", d, p)[0]
    props = bytes(d[p + hdr:p + hdr + 5])
    payload = bytes(d[p + hdr + 5:p + hdr + 5 + cs])
    res = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(props + struct.pack("<Q", usize) + payload)
    if len(res) != usize:
        raise ValueError("blocco LZMA incompleto")
    return res


def parse_igz(raw):
    """Intestazione, pool di memoria e tipi (TMET con dimensioni MTSZ) di un igz."""
    sig, ver, fh, plat, _nfix = struct.unpack_from("<5I", raw, 0)
    if sig != IGZ_SIGNATURE:
        return None
    chunks = []
    off = 0x14
    while off + 16 <= len(raw) and len(chunks) < 64:
        ident, coff, csize, align = struct.unpack_from("<4i", raw, off)
        if coff == 0:
            break
        chunks.append((ident, coff, csize))
        off += 16
    pools, p, last = [], 0x224, None
    for ident, _, _ in chunks:
        if last is not None and last[0] == ident:
            pools.append(last[1])
            continue
        e = raw.find(b"\0", p)
        name = raw[p:e].decode("ascii", "replace")
        p = e + 1
        pools.append(name)
        last = (ident, name)
    types, sizes = [], []
    if chunks:
        pos = chunks[0][1]
        for _ in range(64):
            if pos + 16 > len(raw):
                break
            tag = raw[pos:pos + 4]
            if 0 in tag:
                break
            count, fsize, hsize = struct.unpack_from("<3i", raw, pos + 4)
            if fsize <= 0:
                break
            q = pos + hsize
            if tag == b"TMET":
                for _ in range(count):
                    e = raw.find(b"\0", q)
                    types.append(raw[q:e].decode("ascii", "replace"))
                    q = e + 1
                    if q % 2:
                        q += 1
            elif tag == b"MTSZ":
                sizes = list(struct.unpack_from("<%di" % count, raw, q))
            pos += fsize
    return {"versione": ver, "piattaforma": plat, "campo3": fh, "pool": pools,
            "tipi": dict(zip(types, sizes)) if len(sizes) == len(types) else {t: None for t in types}}


def describe_hkx(raw):
    if raw[:8] == HKX_MAGIC:
        ver = raw[0x28:0x38].split(b"\0")[0].decode("ascii", "replace")
        return "packfile %s, puntatori %d byte, %s" % (ver, raw[16], "little-endian" if raw[17] else "big-endian")
    if raw[4:8] in (b"TAG0", b"TCM0"):
        return "tagfile " + raw[4:8].decode()
    return "sconosciuto (%s)" % raw[:8].hex()


def level_names(paths):
    out = []
    for _, short in paths:
        m = re.match(r"packages/generated/maps/([^/]+)/([^/]+)/([^/]+)_pkg\.igz$", short, re.I)
        if m:
            out.append((m.group(1), m.group(3)))
    return out


def ids_scheme(pak):
    sample = list(range(min(pak.n, 200)))
    ok = sum(1 for i in sample if pak.ids[i] == fnv1a(pak.paths[i][1]))
    ordered = all(pak.ids[i] <= pak.ids[i + 1] for i in range(pak.n - 1))
    if ok == len(sample):
        return "FNV-1a del percorso breve" + (", ordinati" if ordered else ", NON ordinati")
    return "schema diverso (%d/%d coincidono con FNV-1a)" % (ok, len(sample))


def root_of(pak):
    roots = Counter()
    for full, short in pak.paths:
        if full.endswith(short) and len(full) > len(short):
            roots[full[:-len(short)]] += 1
    return roots.most_common(1)[0][0] if roots else "(nessuna)"


def main():
    ap = argparse.ArgumentParser(description="Controlla un .pak prima di installarlo sulla Switch")
    ap.add_argument("mio")
    ap.add_argument("--switch", required=True, help="cartella del dump RomFS (o un .pak) della Switch")
    ap.add_argument("--come", help="nome con cui lo installi (default: nome del file)")
    ap.add_argument("--max-igz", type=int, default=3000, help="massimo numero di igz da confrontare")
    ap.add_argument("-o", "--output", help="file del rapporto (default: report_<nome>.txt)")
    a = ap.parse_args()

    try:
        mine = Pak(a.mio)
    except Exception as e:
        sys.exit("%s: %s" % (a.mio, e))

    # indice degli originali Switch: percorso breve -> (.pak, indice)
    sw_files = []
    if os.path.isdir(a.switch):
        for root, _, files in os.walk(a.switch):
            sw_files += [os.path.join(root, f) for f in files if f.lower().endswith(".pak")]
    else:
        sw_files = [a.switch]
    if not sw_files:
        sys.exit("Nessun .pak in " + a.switch)
    print("Indicizzo %d archivi Switch..." % len(sw_files), file=sys.stderr)
    index, sw_paks, bad = {}, {}, []
    for p in sorted(sw_files):
        try:
            pk = Pak(p)
        except Exception as e:
            bad.append("%s: %s" % (os.path.basename(p), e))
            continue
        sw_paks[p] = pk
        for i, (_, short) in enumerate(pk.paths):
            index.setdefault(short.lower(), (p, i))

    # livello e riferimento
    mine_levels = level_names(mine.paths)
    sw_levels = {}
    for p, pk in sw_paks.items():
        for game, lvl in level_names(pk.paths):
            sw_levels[lvl.lower()] = (game, lvl, p)
    rename = None
    ref_pak = None
    if mine_levels:
        game, lvl = mine_levels[0]
        if lvl.lower() in sw_levels:
            ref_pak = sw_paks[sw_levels[lvl.lower()][2]]
        else:
            # livello nuovo creato da uno originale: L112_RoadToNowhere_Custom -> L112_RoadToNowhere
            cands = [v for k, v in sw_levels.items() if lvl.lower().startswith(k)]
            if cands:
                g2, orig, p = max(cands, key=lambda v: len(v[1]))
                rename = (lvl, orig)
                ref_pak = sw_paks[p]
    if ref_pak is None and len(sw_paks) == 1:
        ref_pak = next(iter(sw_paks.values()))

    def counterpart(short):
        cands = [short]
        if short.lower().startswith("update/"):
            cands.append(short[7:])  # l'editor li unisce a update.pak senza il prefisso
        for c in cands:
            if c.lower() in index:
                return index[c.lower()], "stesso percorso"
        if rename:
            for c in cands:
                k2 = re.sub(re.escape(rename[0]), rename[1], c, flags=re.I).lower()
                if k2 in index:
                    return index[k2], "con il nome del livello originale"
        return None, None

    if ref_pak is None:
        # nessun livello: l'archivio Switch con piu' file in comune
        hits = Counter(counterpart(s)[0][0] for _, s in mine.paths if counterpart(s)[0])
        if hits:
            ref_pak = sw_paks[hits.most_common(1)[0][0]]

    print("Confronto i file...", file=sys.stderr)
    stats = Counter()
    where = Counter()
    diff_files, missing = [], []
    my_types, sw_types = defaultdict(set), defaultdict(set)
    my_plat, sw_plat, my_pools, sw_pools = Counter(), Counter(), Counter(), Counter()
    hkx_mine, hkx_sw = Counter(), Counter()
    diff_types = Counter()
    errors = Counter()
    igz_done = 0
    update_files = [s for _, s in mine.paths if s.lower().startswith("update/")]

    for i, (_, short) in enumerate(mine.paths):
        low = short.lower()
        (cp, how) = counterpart(short)
        if cp:
            where[how] += 1
        else:
            missing.append(short)
        try:
            if low.endswith(".igz"):
                if igz_done >= a.max_igz:
                    stats["igz non letti (limite)"] += 1
                    continue
                igz_done += 1
                raw = mine.read(i)
                info = parse_igz(raw)
                if not info:
                    errors["igz con firma sconosciuta"] += 1
                    continue
                my_plat[info["piattaforma"]] += 1
                my_pools.update(set(info["pool"]))
                for t, s in info["tipi"].items():
                    my_types[t].add(s)
                if not cp:
                    continue
                spak = sw_paks[cp[0]]
                sraw = spak.read(cp[1])
                sinfo = parse_igz(sraw)
                if not sinfo:
                    continue
                sw_plat[sinfo["piattaforma"]] += 1
                sw_pools.update(set(sinfo["pool"]))
                for t, s in sinfo["tipi"].items():
                    sw_types[t].add(s)
                if raw == sraw:
                    stats["igz identici"] += 1
                elif len(raw) == len(sraw) and raw[16:] == sraw[16:] and raw[:8] == sraw[:8]:
                    stats["igz identici tranne intestazione (piattaforma/campo3)"] += 1
                else:
                    same_types = info["tipi"] == sinfo["tipi"]
                    stats["igz diversi" + (" (stessi tipi e dimensioni)" if same_types else " (tipi diversi)")] += 1
                    for t in info["tipi"]:
                        diff_types[t] += 1
                    diff_files.append((short, len(raw), len(sraw), same_types, sorted(info["tipi"])[:6]))
            elif low.endswith(".hkx"):
                d1 = describe_hkx(mine.read(i, limit=0x8000))
                hkx_mine[d1] += 1
                if cp:
                    sraw = sw_paks[cp[0]].read(cp[1], limit=0x8000)
                    hkx_sw[describe_hkx(sraw)] += 1
                    full_mine, full_sw = mine.read(i), sw_paks[cp[0]].read(cp[1])
                    stats["hkx identici" if full_mine == full_sw else "hkx diversi"] += 1
            elif cp:
                same = mine.read(i) == sw_paks[cp[0]].read(cp[1])
                stats["altri file identici" if same else "altri file diversi"] += 1
        except Exception as e:
            errors[type(e).__name__ + ": " + str(e)[:60]] += 1

    out = []
    w = out.append
    w("RAPPORTO controlla_pak.py")
    w("da installare: %s   originali Switch: %s (%d archivi)" % (mine.name, a.switch, len(sw_paks)))
    if bad:
        w("archivi Switch non letti: %s" % "; ".join(bad[:5]))
    w("")
    w("=== CONTENITORE ===")
    ref = ref_pak
    w("%-20s %-38s %s" % ("", "da installare", "Switch (%s)" % (ref.name if ref else "-")))
    for k, label in enumerate(HEADER_FIELDS):
        if label in ("dim. indice", "file", "blocchi grandi", "blocchi medi", "blocchi piccoli",
                     "offset percorsi", "dim. percorsi"):
            continue
        v1 = mine.header[k]
        v2 = ref.header[k] if ref else "-"
        if label == "firma":
            v1, v2 = "0x%08X" % v1, ("0x%08X" % v2 if ref else "-")
        w("%-20s %-38s %s" % (label, v1, v2))
    w("%-20s %-38s %s" % ("radice percorsi", root_of(mine), root_of(ref) if ref else "-"))
    w("%-20s %-38s %s" % ("header LZMA (byte)", mine.lzma_header(), ref.lzma_header() if ref else "-"))
    w("%-20s %-38s %s" % ("compressione", dict(Counter(COMP.get(mine.comp(i), "?") for i in range(mine.n))),
                          dict(Counter(COMP.get(ref.comp(i), "?") for i in range(ref.n))) if ref else "-"))
    w("%-20s %-38s %s" % ("ID dei file", ids_scheme(mine), ids_scheme(ref) if ref else "-"))
    w("%-20s %-38s %s" % ("igz piattaforma", dict(my_plat), dict(sw_plat)))
    w("%-20s %-38s %s" % ("havok", dict(hkx_mine), dict(hkx_sw)))
    w("pool di memoria solo nel mio: %s" % sorted(set(my_pools) - set(sw_pools)))
    w("pool di memoria solo Switch:  %s" % sorted(set(sw_pools) - set(my_pools)))
    w("")
    w("=== LIVELLO ===")
    w("dentro: %s" % (", ".join("%s/%s" % l for l in mine_levels) or "nessun livello"))
    if rename:
        w("creato dal livello originale: %s (nella Switch: %s)" % (rename[1], ref_pak.name if ref_pak else "?"))
    w("file da unire a update.pak (update/...): %d %s" % (len(update_files), update_files[:8]))
    w("")
    w("=== FILE CONFRONTATI ===")
    w("file nel .pak: %d; con corrispondente Switch: %s; senza: %d" % (mine.n, dict(where), len(missing)))
    for k in sorted(stats):
        w("  %-55s %d" % (k, stats[k]))
    if errors:
        w("errori: %s" % dict(errors))
    w("")
    w("=== TIPI DI OGGETTO (dimensione in byte) ===")
    both = sorted(set(my_types) & set(sw_types))
    size_diff = [(t, sorted(my_types[t], key=str), sorted(sw_types[t], key=str)) for t in both if my_types[t] != sw_types[t]]
    w("tipi in comune: %d; con dimensione diversa: %d" % (len(both), len(size_diff)))
    for t, s1, s2 in size_diff[:80]:
        w("  %-50s mio %s  Switch %s" % (t, s1, s2))
    only_mine = sorted(set(my_types) - set(sw_types))
    w("tipi solo nel mio (%d): %s" % (len(only_mine), ", ".join(only_mine[:120])))
    w("tipi presenti nei file diversi: %s" % ", ".join("%s(%d)" % kv for kv in diff_types.most_common(40)))
    w("")
    w("=== FILE DIVERSI (max 80) ===")
    for short, l1, l2, same_types, types in diff_files[:80]:
        w("  %s  [%d / %d byte]%s  %s" % (short, l1, l2, "" if same_types else " tipi diversi", ",".join(types)))
    w("")
    w("=== FILE SENZA CORRISPONDENTE (max 80) ===")
    for s in missing[:80]:
        w("  " + s)

    report = "\n".join(out) + "\n"
    path = a.output or "report_%s.txt" % os.path.splitext(mine.name)[0].replace(" ", "_")
    with open(path, "w", encoding="utf-8") as f:
        f.write(report)

    # riassunto a schermo
    target = a.come or mine.name
    if not target.lower().endswith(".pak"):
        target += ".pak"
    print("\n=== RIASSUNTO ===")
    inside = [l for _, l in mine_levels]
    if inside and os.path.splitext(target)[0].lower() not in [x.lower() for x in inside]:
        print(" * NOME: dentro c'e' il livello '%s', non '%s'." % (inside[0], os.path.splitext(target)[0]))
        if rename:
            print("   E' un livello nuovo creato da %s: serve la registrazione in update.pak," % rename[1])
            print("   oppure va rifatto modificando direttamente %s nell'editor." % rename[1])
    elif inside:
        print(" - nome interno OK (%s)" % inside[0])
    if ref:
        print(" - contenitore: versione %s -> %s, radice %s -> %s, header LZMA %s -> %s" % (
            mine.version, ref.version, root_of(mine), root_of(ref), mine.lzma_header(), ref.lzma_header()))
    print(" - igz piattaforma: %s -> %s" % (dict(my_plat), dict(sw_plat)))
    print(" - file confrontati: %s" % ", ".join("%s %d" % (k, v) for k, v in sorted(stats.items())))
    print(" - tipi con dimensione diversa tra PC e Switch: %d" % len(size_diff))
    print("\nRapporto completo: %s  (allegalo in chat)" % path)

    for pk in sw_paks.values():
        pk.close()
    mine.close()


if __name__ == "__main__":
    main()
