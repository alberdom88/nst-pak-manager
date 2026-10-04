#!/usr/bin/env python3
"""
layout_switch.py - ricava la struttura degli oggetti della versione Switch dai metadati PC dell'editor

Ipotesi: PC e Switch hanno le stesse classi con gli stessi campi nello stesso ordine; cambia
solo il compilatore. Il PC usa le regole di MSVC (la classe derivata inizia dopo la dimensione
completa della base), la Switch quelle Itanium/clang (i campi della derivata possono occupare
il padding finale della base, quindi igObject "pesa" 12 byte invece di 16).

Uso:
    python layout_switch.py <cartella src dell'editor> [tipi.json] [-o layout_switch.json]

Senza tipi.json calcola solo il modello PC e lo confronta con le dimensioni note all'editor
(serve a verificare il modello). Con tipi.json confronta le dimensioni Switch calcolate con
quelle reali e scrive, per ogni classe, gli offset dei campi PC -> Switch.
"""

import argparse
import json
import os
import re
import sys
from collections import Counter

PRIMS = {
    "bool": (1, 1), "byte": (1, 1), "sbyte": (1, 1), "u8": (1, 1), "i8": (1, 1),
    "short": (2, 2), "ushort": (2, 2), "u16": (2, 2), "i16": (2, 2), "Half": (2, 2), "char": (2, 2),
    "int": (4, 4), "uint": (4, 4), "u32": (4, 4), "i32": (4, 4), "float": (4, 4),
    "long": (8, 8), "ulong": (8, 8), "u64": (8, 8), "i64": (8, 8), "double": (8, 8),
    "string": (8, 8),
    "Vector4": (16, 16), "System.Numerics.Vector4": (16, 16),
    "Quaternion": (16, 16), "System.Numerics.Quaternion": (16, 16),
    "Matrix4x4": (64, 16), "System.Numerics.Matrix4x4": (64, 16),
    "Matrix3x4": (48, 16),
}
ENUM_BASE = {"byte": 1, "sbyte": 1, "short": 2, "ushort": 2, "int": 4, "uint": 4, "long": 8, "ulong": 8}

CLASS_RE = re.compile(r"\b(?:public|internal|private|protected)?\s*(?:abstract\s+|sealed\s+|static\s+|partial\s+)*"
                      r"(class|struct|enum)\s+(\w+)(<[^>{]*>)?\s*(?::\s*([\w\.<>,\? ]+?))?\s*(?:where[^{]*)?\{")


def arity(generic):
    """Numero di parametri di tipo in '<A, B<C, D>>' (solo il livello esterno)."""
    if not generic:
        return 0
    depth, n = 0, 1
    for ch in generic[1:-1]:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == "," and depth == 0:
            n += 1
    return n
ATTR_RE = re.compile(r"\[(ObjectAttr|FieldAttr)\(([^\]]*)\)\]")
FIELD_RE = re.compile(r"\[FieldAttr\(([^\]]*)\)\]\s*public\s+([\w\.<>,\?\[\] ]+?)\s+(\w+)\s*(?:=|;)")


def parse_args(text, names):
    """Argomenti di un attributo: posizionali (nell'ordine di names) e con nome."""
    out = {}
    pos = 0
    for part in [p.strip() for p in text.split(",") if p.strip()]:
        m = re.match(r"(\w+)\s*:\s*(.+)$", part)
        if m:
            key, val = m.group(1), m.group(2)
        else:
            if pos >= len(names):
                continue
            key, val = names[pos], part
            pos += 1
        try:
            out[key] = int(val, 0)
        except ValueError:
            out[key] = val
    return out


class Cls:
    def __init__(self, name, full, base, kind):
        self.name, self.full, self.base, self.kind = name, full, base, kind
        self.attr = {}
        self.fields = []   # (nome, tipo, nst, ctr, bits)
        self.enum_size = 4


def parse_sources(src):
    classes = {}
    for root, _, files in os.walk(src):
        for f in files:
            if not f.endswith(".cs"):
                continue
            text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
            text = re.sub(r"//[^\n]*", "", text)
            stack = []          # (classe, profondita' di parentesi all'apertura)
            depth = 0
            pending_attr = None
            i = 0
            while i < len(text):
                m = CLASS_RE.match(text, i) if text[i] in "pcsei" or text[i].isspace() else None
                if m and (i == 0 or not text[i - 1].isalnum()):
                    kind, name, base = m.group(1), m.group(2), (m.group(4) or "").strip()
                    n = arity(m.group(3))
                    outer = stack[-1][0].full + "." if stack else ""
                    c = Cls(name + ("`%d" % n if n else ""), outer + name + ("`%d" % n if n else ""), base, kind)
                    if kind == "enum":
                        c.enum_size = ENUM_BASE.get(base, 4)
                    # ObjectAttr subito prima della dichiarazione
                    back = text[max(0, i - 300):i]
                    am = list(re.finditer(r"\[ObjectAttr\(([^\]]*)\)\]\s*$", back))
                    if am:
                        c.attr = parse_args(am[-1].group(1), ["size", "nst", "ctr", "align"])
                        if "size" in c.attr:
                            # [ObjectAttr(40, 8)]: dimensione 40, allineamento 8
                            if "nst" in c.attr and "align" not in c.attr:
                                c.attr["align"] = c.attr["nst"]
                            c.attr["nst"] = c.attr["ctr"] = c.attr["size"]
                    classes.setdefault(c.full, c)
                    depth += 1
                    stack.append((c, depth))
                    i = m.end()
                    continue
                ch = text[i]
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    if stack and stack[-1][1] == depth:
                        stack.pop()
                    depth -= 1
                elif ch == "[" and text.startswith("[FieldAttr(", i) and stack:
                    fm = FIELD_RE.match(text, i)
                    if fm:
                        a = parse_args(fm.group(1), ["offset", "nst", "ctr", "size", "refCount"])
                        nst = a.get("nst", a.get("offset"))
                        ctr = a.get("ctr", a.get("offset"))
                        stack[-1][0].fields.append((fm.group(3), fm.group(2).strip(), nst, ctr, a.get("size")))
                        i = fm.end()
                        continue
                i += 1
    return classes


class Model:
    def __init__(self, classes):
        self.c = classes
        self.by_name = {}
        for full, c in classes.items():
            self.by_name.setdefault(c.name, []).append(c)
        self.cache = {}
        # allineamento delle strutture ricavato dagli offset PC in cui compaiono
        self.obs_align = {}
        for c in classes.values():
            for name, t, nst, ctr, bits in c.fields:
                if nst is None or bits is not None:
                    continue
                key = self._norm(t)
                p = 16
                while p > 1 and nst % p:
                    p //= 2
                self.obs_align[key] = min(self.obs_align.get(key, 16), p)

    @staticmethod
    def _norm(t):
        t = re.sub(r"\s+", "", t).rstrip("?")
        g = re.search(r"<.*>", t)
        return re.sub(r"<.*>", "`%d" % arity(g.group(0)), t) if g else t

    def find(self, name, ctx=None):
        name = name.strip().rstrip("?")
        g = re.search(r"<.*>", name)
        n = arity(g.group(0)) if g else 0
        name = re.sub(r"<.*>", "", name).strip()
        if n:
            name += "`%d" % n
        if ctx:
            # classe annidata: prima nel contesto della classe che la usa
            parts = ctx.full.split(".")
            for k in range(len(parts), 0, -1):
                cand = ".".join(parts[:k]) + "." + name
                if cand in self.c:
                    return self.c[cand]
        if "." in name and name in self.c:
            return self.c[name]
        lst = self.by_name.get(name.split(".")[-1])
        return lst[0] if lst else None

    def is_object(self, c, seen=0):
        while c and seen < 50:
            if c.name in ("igObject", "hkReferencedObject"):
                return True
            c = self.find(c.base) if c.base else None
            seen += 1
        return False

    def type_info(self, t, ctx):
        """(dimensione, allineamento) del tipo t, o None se sconosciuto."""
        t = t.strip().rstrip("?")
        if t.endswith("[]"):
            return None
        if t in PRIMS:
            return PRIMS[t]
        c = self.find(t, ctx)
        if not c:
            return None
        if c.kind == "enum":
            return (c.enum_size, c.enum_size)
        if self.is_object(c):
            return (8, 8)  # puntatore
        size = c.attr.get("nst")
        if size is None:
            return None
        al = c.attr.get("align", 4)
        obs = self.obs_align.get(self._norm(t))
        if obs:
            al = min(obs, max(8, c.attr.get("align", 0)))
        return (size, al)

    def layout(self, cname, itanium):
        key = (cname, itanium)
        if key in self.cache:
            return self.cache[key]
        self.cache[key] = None  # protezione da cicli
        c = self.c.get(cname)
        if not c:
            return None
        if c.name == "igObject":
            res = {"dsize": 12 if itanium else 16, "size": 16, "align": 8, "offsets": {}, "ok": True}
            self.cache[key] = res
            return res
        base = self.find(c.base) if c.base else None
        if base and self.is_object(base):
            b = self.layout(base.full, itanium)
            if not b:
                return None
            cur = b["dsize"] if itanium else b["size"]
            align = b["align"]
            offsets = dict(b["offsets"])
            ok = b["ok"]
        else:
            return None  # non e' un igObject
        own = [f for f in c.fields if f[2] is not None]
        own.sort(key=lambda f: f[2])
        for name, t, nst, ctr, bits in own:
            ti = self.type_info(t, c)
            if ti is None:
                ok = False
                ti = (8, 8)
            size, al = ti
            off = (cur + al - 1) // al * al
            offsets[c.full + "." + name] = (nst, off, size)
            cur = off + size
            align = max(align, al)
        align = max(align, c.attr.get("align", 1))
        size = (cur + align - 1) // align * align
        res = {"dsize": cur, "size": size, "align": align, "offsets": offsets, "ok": ok}
        self.cache[key] = res
        return res


def lowbit(n, cap=8):
    p = 1
    while p < cap and n % (p * 2) == 0:
        p *= 2
    return p


def align_up(x, a):
    return (x + a - 1) // a * a


def switch_layout(m, cname, cache={}):
    """Layout Itanium della classe, conservando i membri nascosti (buchi negli offset PC)."""
    if cname in cache:
        return cache[cname]
    cache[cname] = None
    c = m.c.get(cname)
    if not c:
        return None
    if c.name == "igObject":
        res = {"dsize": 12, "size": 16, "align": 8, "offsets": {}, "ok": True}
        cache[cname] = res
        return res
    base = m.find(c.base) if c.base else None
    if not base or not m.is_object(base):
        return None
    b = switch_layout(m, base.full)
    if not b:
        return None
    cur, align, offsets, ok = b["dsize"], b["align"], dict(b["offsets"]), b["ok"]
    pc_end = base.attr.get("nst") if base.name != "igObject" else 16
    if pc_end is None:
        r = m.layout(base.full, itanium=False)
        pc_end = r["size"] if r else 16
    own = sorted([f for f in c.fields if f[2] is not None], key=lambda f: f[2])
    for name, t, nst, ctr, bits in own:
        ti = m.type_info(t, c)
        if ti is None:
            ok = False
            ti = (8, 8)
        size, al = ti
        if nst > align_up(pc_end, al):
            gap = nst - pc_end           # membri che l'editor non descrive
            ga = lowbit(gap)
            cur = align_up(cur, ga) + gap
            align = max(align, ga)
        off = align_up(cur, al)
        offsets[c.full + "." + name] = (nst, off, size)
        cur = off + size
        pc_end = nst + size
        align = max(align, al)
    pc_size = c.attr.get("nst")
    pc_align = max([align] + [c.attr.get("align", 1)])
    if pc_size and pc_size > align_up(pc_end, pc_align):
        gap = pc_size - pc_end
        ga = lowbit(gap)
        cur = align_up(cur, ga) + gap
        align = max(align, ga)
    align = max(align, c.attr.get("align", 1))
    res = {"dsize": cur, "size": align_up(cur, align), "align": align, "offsets": offsets, "ok": ok}
    cache[cname] = res
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("tipi", nargs="?")
    ap.add_argument("-o", "--output", default="layout_switch.json")
    a = ap.parse_args()

    classes = parse_sources(a.src)
    m = Model(classes)
    objs = [c for c in classes.values() if c.kind == "class" and "nst" in c.attr and m.is_object(c)]

    # 1. verifica del modello sul PC: le dimensioni MSVC calcolate devono coincidere con quelle dell'editor
    pc = Counter()
    pc_bad = []
    for c in objs:
        r = m.layout(c.full, itanium=False)
        if not r:
            pc["non calcolabile"] += 1
        elif r["size"] == c.attr["nst"] and r["ok"]:
            pc["uguale"] += 1
        else:
            # gli offset dei campi PC devono coincidere con quelli calcolati
            pc["diversa" if r["ok"] else "tipi di campo sconosciuti"] += 1
            pc_bad.append((c.name, c.attr["nst"], r["size"]))
    print("MODELLO PC (MSVC) contro dimensioni dell'editor:", dict(pc))
    offs_ok = offs_bad = 0
    for c in objs:
        r = m.layout(c.full, itanium=False)
        if not r or not r["ok"]:
            continue
        for k, (nst, off, size) in r["offsets"].items():
            if nst == off:
                offs_ok += 1
            else:
                offs_bad += 1
    print("  offset dei campi PC riprodotti: %d esatti, %d diversi" % (offs_ok, offs_bad))
    if pc_bad:
        print("  esempi:", pc_bad[:12])

    if not a.tipi:
        return
    sw = {t: int(next(iter(v))) for t, v in json.load(open(a.tipi))["switch"]["tipi"].items()}
    res = Counter()
    bad = []
    out = {}
    for name, size in sorted(sw.items()):
        cands = [c for c in m.by_name.get(name, []) if c.kind == "class" and m.is_object(c)]
        if not cands:
            res["classe sconosciuta all'editor"] += 1
            continue
        c = cands[0]
        r = switch_layout(m, c.full)
        if not r:
            res["non calcolabile"] += 1
            continue
        if not r["ok"]:
            res["tipi di campo sconosciuti"] += 1
        if r["size"] == size:
            res["dimensione Switch esatta"] += 1
        else:
            res["dimensione Switch diversa"] += 1
            bad.append((name, c.attr.get("nst"), r["size"], size))
        out[name] = {"pc": c.attr.get("nst"), "switch": size, "calcolata": r["size"],
                     "campi": {k.split(".")[-1]: [nst, off, sz] for k, (nst, off, sz) in r["offsets"].items()}}
    print("MODELLO SWITCH (Itanium) contro dimensioni reali:", dict(res))
    print("  diverse (classe, pc, calcolata, reale):")
    for b in bad[:40]:
        print("   ", b)
    with open(a.output, "w") as f:
        json.dump(out, f, indent=0)
    print("scritto", a.output)


if __name__ == "__main__":
    main()
