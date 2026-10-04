#!/usr/bin/env python3
"""
analizza_avvio.py - come la versione Switch del gioco usa le opzioni di avvio (-om)

Legge l'eseguibile del gioco (file "main" della ExeFS, formato NSO) e scrive
solo NOMI di funzioni e TESTI: chi legge gli argomenti di avvio, chi usa
l'opzione "-om", la descrizione dell'opzione e le funzioni che caricano il
primo livello. Non copia istruzioni del gioco.

Uso:
  python analizza_avvio.py "<file main oppure cartella del dump ExeFS>"
      scrive avvio.txt
  python analizza_avvio.py "<file main oppure cartella>" <testo> [<testo> ...]
      cerca le funzioni il cui nome contiene i testi indicati e scrive funzioni.txt
  python analizza_avvio.py "<file main oppure cartella>" --config
      da dove viene la configurazione letta all'avvio (MAP/filename) e scrive configurazione.txt

Allega in chat il file prodotto.
"""

import array
import bisect
import os
import re
import struct
import sys

try:
    import lz4.block as _lz4  # facoltativo: piu' veloce (pip install lz4)
except Exception:  # noqa: BLE001
    _lz4 = None


# ---------------------------------------------------------------- NSO

def lz4_block_decompress(src, size):
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
        raise ValueError("dimensione decompressa sbagliata")
    return bytes(dst)


class Module:
    def __init__(self, data):
        if data[:4] != b"NSO0":
            raise ValueError("non e' un file NSO")
        flags = struct.unpack_from("<I", data, 0x0C)[0]
        self.build_id = data[0x40:0x60].hex().upper()
        segs = []
        for k in range(3):
            file_off, mem_off, size = struct.unpack_from("<3I", data, 0x10 + 0x10 * k)
            file_size = struct.unpack_from("<I", data, 0x60 + 4 * k)[0]
            raw = data[file_off:file_off + file_size]
            if flags & (1 << k) and len(raw) != size:
                raw = lz4_block_decompress(raw, size)
            segs.append((mem_off, raw[:size]))
        end = max(m + len(r) for m, r in segs)
        self.mem = bytearray(end)
        for m, r in segs:
            self.mem[m:m + len(r)] = r
        self.text = (segs[0][0], segs[0][0] + len(segs[0][1]))
        self.ro = (segs[1][0], segs[1][0] + len(segs[1][1]))
        self.data = (segs[2][0], segs[2][0] + len(segs[2][1]))
        self.bss_end = self.data[1] + struct.unpack_from("<I", data, 0x3C)[0]
        self._dynamic()

    def u32(self, a):
        return struct.unpack_from("<I", self.mem, a)[0]

    def u64(self, a):
        return struct.unpack_from("<Q", self.mem, a)[0]

    def cstr(self, a, limit=300):
        if a < 0 or a >= len(self.mem):
            return None
        end = self.mem.find(b"\0", a, a + limit)
        if end <= a:
            return None
        s = self.mem[a:end]
        if not all(32 <= c < 127 for c in s):
            return None
        return s.decode("ascii")

    def _dynamic(self):
        mod0 = self.u32(4)
        if self.mem[mod0:mod0 + 4] != b"MOD0":
            raise ValueError("intestazione MOD0 non trovata")
        dyn = mod0 + struct.unpack_from("<i", self.mem, mod0 + 4)[0]
        tags = {}
        a = dyn
        while a + 16 <= len(self.mem):
            tag, val = struct.unpack_from("<qQ", self.mem, a)
            a += 16
            if tag == 0:
                break
            tags.setdefault(tag, val)
        symtab, strtab = tags.get(6), tags.get(5)
        if symtab is None or strtab is None:
            raise ValueError("tabella dei simboli non trovata")
        if 4 in tags:
            count = self.u32(tags[4] + 4)
        else:
            count = (strtab - symtab) // 24 if strtab > symtab else 0
        self.syms = []
        for i in range(count):
            name_off, info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.mem, symtab + 24 * i)
            name = self.cstr(strtab + name_off, 1000) or ""
            self.syms.append((name, info & 0xF, shndx, value, size))
        # funzioni definite: indirizzo -> nome
        funcs = sorted((v, v + max(s, 4), n) for n, t, sh, v, s in self.syms if t == 2 and sh != 0 and v)
        self.func_starts = [f[0] for f in funcs]
        self.funcs = funcs
        self.func_at = {f[0]: f[2] for f in funcs}
        # variabili globali e tabelle virtuali (_ZTV...): indirizzo -> nome
        self.objects = {v: n for n, t, sh, v, s in self.syms if t == 1 and sh != 0 and v and n}
        # rilocazioni: GOT -> simbolo importato, puntatori relativi -> destinazione
        self.got = {}
        self.relative = {}
        for start_tag, size_tag in ((23, 2), (7, 8)):
            start, size = tags.get(start_tag), tags.get(size_tag)
            if start is None or size is None:
                continue
            for k in range(size // 24):
                off, info, addend = struct.unpack_from("<QQq", self.mem, start + 24 * k)
                rtype, sym = info & 0xFFFFFFFF, info >> 32
                if rtype in (1025, 1026, 257) and sym < len(self.syms):  # GLOB_DAT, JUMP_SLOT, ABS64
                    self.got[off] = self.syms[sym][0]
                elif rtype == 1027:  # RELATIVE
                    self.relative[off] = addend

    def function_of(self, addr):
        i = bisect.bisect_right(self.func_starts, addr) - 1
        if i >= 0 and self.funcs[i][0] <= addr < self.funcs[i][1]:
            return self.funcs[i][2]
        return None

    def find_functions(self, part):
        return [f for f in self.funcs if part in f[2]]

    def is_data(self, addr):
        return self.data[0] <= addr < self.bss_end


# ---------------------------------------------------------------- AArch64

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Scanner:
    def __init__(self, m):
        self.m = m
        self.stub_cache = {}

    def call_name(self, target):
        """Nome della funzione chiamata (anche attraverso lo stub PLT)"""
        m = self.m
        if target in m.func_at:
            return m.func_at[target]
        if target in self.stub_cache:
            return self.stub_cache[target]
        name = None
        if m.text[0] <= target and target + 16 <= m.text[1]:
            i0, i1 = m.u32(target), m.u32(target + 4)
            if (i0 & 0x9F00001F) == 0x90000010 and (i1 & 0xFFC003FF) == 0xF9400211:
                page = (target & ~0xFFF) + (sext(((i0 >> 5) & 0x7FFFF) << 2 | ((i0 >> 29) & 3), 21) << 12)
                got = page + ((i1 >> 10) & 0xFFF) * 8
                name = m.got.get(got)
        if name is None:
            name = m.function_of(target)
            if name is not None and target not in m.func_at:
                name = None
        self.stub_cache[target] = name
        return name

    def scan(self, start, end, on_ref=None, on_call=None):
        """Scorre le istruzioni: indirizzi calcolati (adrp+add/ldr) e chiamate (bl/b)"""
        m = self.m
        regs = {}
        words = array.array("I")
        words.frombytes(bytes(m.mem[start:end]))
        if sys.byteorder != "little":
            words.byteswap()
        pc = start - 4
        for insn in words:
            pc += 4
            if (insn & 0x9F000000) == 0x90000000:  # ADRP
                imm = sext(((insn >> 5) & 0x7FFFF) << 2 | ((insn >> 29) & 3), 21)
                regs[insn & 31] = (pc & ~0xFFF) + (imm << 12)
            elif (insn & 0x7F000000) == 0x11000000:  # ADD immediata (64 bit)
                rn, rd = (insn >> 5) & 31, insn & 31
                if rn in regs:
                    imm = ((insn >> 10) & 0xFFF) << (12 if (insn >> 22) & 1 else 0)
                    regs[rd] = regs[rn] + imm
                    if on_ref:
                        on_ref(pc, regs[rd])
                else:
                    regs.pop(rd, None)
            elif (insn & 0x3F000000) == 0x39000000:  # LDR/STR (anche w, h, b) [x, #imm]
                rn, rt = (insn >> 5) & 31, insn & 31
                size = insn >> 30
                if rn in regs and on_ref:
                    on_ref(pc, regs[rn] + (((insn >> 10) & 0xFFF) << size))
                if (insn >> 22) & 3:  # lettura: il registro cambia
                    regs.pop(rt, None)
            elif (insn & 0xFC000000) in (0x94000000, 0x14000000):  # BL / B
                target = pc + sext(insn & 0x3FFFFFF, 26) * 4
                if on_call:
                    on_call(pc, target)
                if (insn & 0xFC000000) == 0x94000000:
                    for r in range(0, 19):
                        regs.pop(r, None)


# ---------------------------------------------------------------- analisi

KEY_FUNCTIONS = [
    "parseCommandLineArgs", "loadStartupMap", "loadFirstMap", "getStartZoneInfo",
    "igCommandLine9parseFile", "igCommandLine5parseEiPPKci", "igCommandLine5parseEPci",
    "LoadMapFunc", "nnMain",
]


def describe_function(m, sc, start, end):
    strings, calls = [], []

    def on_ref(pc, addr):
        if m.ro[0] <= addr < m.ro[1] or m.data[0] <= addr < m.data[1]:
            s = m.cstr(addr)
            if s and len(s) >= 2 and s not in strings:
                strings.append(s)
            target = m.relative.get(addr)
            if target is not None:
                s2 = m.cstr(target)
                if s2 and len(s2) >= 2 and s2 not in strings:
                    strings.append("(puntatore) " + s2)

    def on_call(pc, target):
        name = sc.call_name(target)
        if name and name not in calls:
            calls.append(name)

    sc.scan(start, end, on_ref, on_call)
    return strings, calls


CONDITIONS = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]

OUTLINE_FUNCTIONS = ["loadStartupMap", "gameMainInitialize", "loadExtraArguments", "_Z6igMainiPPc"]


def _fmt(d, depth=0):
    """Descrizione leggibile del valore di un registro"""
    kind = d[0]
    if depth > 4:
        return "..."
    if kind == "arg":
        return "x%d" % d[1]
    if kind == "ret":
        return "risultato di %s" % d[1]
    if kind == "glob":
        return d[1]
    if kind == "addr":
        return "&" + d[1]
    if kind == "load":
        return "[%s + 0x%X]" % (_fmt(d[1], depth + 1), d[2])
    if kind == "method":
        return "metodo virtuale 0x%X di (%s)" % (d[2], _fmt(d[1], depth + 1))
    return "?"


def _name_of_data(m, addr):
    if addr in m.objects:
        return m.objects[addr]
    if addr in m.func_at:
        return m.func_at[addr]
    return "0x%X" % addr


def outline(m, sc, start, end, ranges=None, vtables=None):
    """Schema di una funzione: chiamate, testi, dati globali, confronti e salti.
    ranges: elenco di intervalli (inizio, fine) relativi da stampare; vtables: insieme in cui
    raccogliere le tabelle virtuali (_ZTV...) usate"""
    lines = []
    regs = {}  # registro -> indirizzo calcolato con adrp/add
    desc = {}  # registro -> descrizione del valore (per le chiamate indirette)

    def d_of(r):
        return desc.get(r, ("arg", r))

    def clobber():
        for r in range(0, 19):
            regs.pop(r, None)
            desc.pop(r, None)

    for pc in range(start, end, 4):
        insn = m.u32(pc)
        rel = pc - start
        text = None
        if (insn & 0x9F000000) == 0x90000000:  # ADRP
            imm = sext(((insn >> 5) & 0x7FFFF) << 2 | ((insn >> 29) & 3), 21)
            regs[insn & 31] = (pc & ~0xFFF) + (imm << 12)
            desc.pop(insn & 31, None)
        elif (insn & 0x7F000000) == 0x11000000:  # ADD immediata
            rn, rd = (insn >> 5) & 31, insn & 31
            imm = ((insn >> 10) & 0xFFF) << (12 if (insn >> 22) & 1 else 0)
            if rn in regs:
                addr = regs[rn] + imm
                regs[rd] = addr
                s = m.cstr(addr)
                if s and len(s) >= 2:
                    text = "testo %r -> x%d" % (s, rd)
                    desc[rd] = ("glob", repr(s))
                elif addr in m.objects or addr in m.func_at:
                    name = _name_of_data(m, addr)
                    text = "indirizzo di %s -> x%d" % (name, rd)
                    desc[rd] = ("addr", name)
                    if vtables is not None and name.startswith("_ZTV"):
                        vtables.add(addr)
                elif m.objects.get(addr - 0x10, "").startswith("_ZTV"):  # puntatore alla tabella virtuale
                    name = m.objects[addr - 0x10]
                    text = "tabella virtuale %s -> x%d" % (name, rd)
                    desc[rd] = ("addr", name + "+0x10")
                    if vtables is not None:
                        vtables.add(addr - 0x10)
                elif addr in m.relative and m.cstr(m.relative[addr]):
                    text = "testo %r (puntatore) -> x%d" % (m.cstr(m.relative[addr]), rd)
                else:
                    desc[rd] = ("addr", "0x%X" % addr)
            else:
                regs.pop(rd, None)
                if rd != 31:  # puntatore dentro un oggetto (x + imm)
                    desc[rd] = ("addr", "%s+0x%X" % (_fmt(d_of(rn)), imm)) if imm else d_of(rn)
        elif (insn & 0x3F000000) == 0x39000000:  # LDR/STR [x, #imm]
            rn, rt = (insn >> 5) & 31, insn & 31
            size = insn >> 30
            off = ((insn >> 10) & 0xFFF) << size
            load = (insn >> 22) & 3
            if rn in regs:
                addr = regs[rn] + off
                if load:
                    s = m.cstr(addr) if size == 3 else None
                    if addr in m.got:
                        text = "legge %s -> x%d" % (m.got[addr], rt)
                        desc[rt] = ("addr", m.got[addr])
                    elif addr in m.relative and m.cstr(m.relative[addr]):
                        text = "testo %r (puntatore) -> x%d" % (m.cstr(m.relative[addr]), rt)
                        desc[rt] = ("glob", repr(m.cstr(m.relative[addr])))
                    elif m.is_data(addr):
                        name = _name_of_data(m, addr)
                        text = "legge il dato %s -> x%d" % (name, rt)
                        desc[rt] = ("glob", name)
                    else:
                        desc.pop(rt, None)
                    regs.pop(rt, None)
                elif m.is_data(addr):
                    text = "scrive %s nel dato %s" % ("0" if rt == 31 else _fmt(d_of(rt)), _name_of_data(m, addr))
            elif load:
                base = d_of(rn)
                if size == 3:
                    if base[0] == "load" and base[2] == 0:
                        desc[rt] = ("method", base[1], off)
                    else:
                        desc[rt] = ("load", base, off)
                else:
                    desc.pop(rt, None)
                regs.pop(rt, None)
        elif (insn & 0xFFE0FFE0) == 0xAA0003E0:  # MOV x, x
            rd, rm = insn & 31, (insn >> 16) & 31
            desc[rd] = d_of(rm)
            if rm in regs:
                regs[rd] = regs[rm]
            else:
                regs.pop(rd, None)
        elif (insn & 0xFC000000) == 0x94000000:  # BL
            target = pc + sext(insn & 0x3FFFFFF, 26) * 4
            name = sc.call_name(target) or "0x%X" % target
            text = "chiama %s" % name
            clobber()
            desc[0] = ("ret", name)
        elif (insn & 0xFC000000) == 0x14000000:  # B
            target = pc + sext(insn & 0x3FFFFFF, 26) * 4
            if start <= target < end:
                text = "salta a +0x%X" % (target - start)
            else:
                text = "salta (fine) a %s" % (sc.call_name(target) or "0x%X" % target)
        elif (insn & 0xFF000010) == 0x54000000:  # B.cond
            target = pc + sext((insn >> 5) & 0x7FFFF, 19) * 4
            text = "se %s salta a +0x%X" % (CONDITIONS[insn & 0xF], target - start)
        elif (insn & 0x7E000000) == 0x34000000:  # CBZ / CBNZ
            target = pc + sext((insn >> 5) & 0x7FFFF, 19) * 4
            reg = ("x" if insn >> 31 else "w") + str(insn & 31)
            text = "se %s %s salta a +0x%X" % (reg, "!= 0" if (insn >> 24) & 1 else "== 0", target - start)
        elif (insn & 0x7E000000) == 0x36000000:  # TBZ / TBNZ
            target = pc + sext((insn >> 5) & 0x3FFF, 14) * 4
            bit = ((insn >> 31) << 5) | ((insn >> 19) & 0x1F)
            text = "se bit %d di x%d %s salta a +0x%X" % (bit, insn & 31, "= 1" if (insn >> 24) & 1 else "= 0", target - start)
        elif (insn & 0x7F00001F) == 0x7100001F:  # CMP immediato
            imm = ((insn >> 10) & 0xFFF) << (12 if (insn >> 22) & 1 else 0)
            text = "confronta %s%d con %d" % ("x" if insn >> 31 else "w", (insn >> 5) & 31, imm)
        elif (insn & 0x7F800000) == 0x52800000:  # MOV immediato
            imm = ((insn >> 5) & 0xFFFF) << (16 * ((insn >> 21) & 3))
            if (insn & 31) < 8:
                text = "%s%d = %d" % ("x" if insn >> 31 else "w", insn & 31, imm)
            regs.pop(insn & 31, None)
            desc[insn & 31] = ("glob", str(imm))
        elif (insn & 0xFFFFFC1F) == 0xD63F0000:  # BLR
            r = (insn >> 5) & 31
            text = "chiamata indiretta: %s, con x0 = %s" % (_fmt(d_of(r)), _fmt(d_of(0)))
            clobber()
            desc[0] = ("ret", "chiamata indiretta")
        elif (insn & 0xFFFFFC1F) == 0xD61F0000:  # BR
            text = "salto indiretto (x%d)" % ((insn >> 5) & 31)
        elif insn == 0xD65F03C0:
            text = "ritorna"
        if text and len(text) > 220:
            text = text[:217] + "..."
        if text and (ranges is None or any(a <= rel < b for a, b in ranges)):
            lines.append("  +0x%04X  %s" % (rel, text))
    return lines


def vtable_entries(m, addr, count=48):
    """Voci di una tabella virtuale: offset del metodo (come nelle chiamate) -> nome"""
    out = []
    misses = 0
    for k in range(count):
        slot = addr + 0x10 + 8 * k
        if k and slot in m.objects:  # inizia un altro dato: fine della tabella
            break
        if slot in m.relative:
            tgt = m.relative[slot]
            name = m.func_at.get(tgt) or m.function_of(tgt) or "0x%X" % tgt
        elif slot in m.got:
            name = m.got[slot]
        else:
            misses += 1
            if misses > 3:
                break
            continue
        misses = 0
        out.append("     0x%03X: %s" % (8 * k, name))
    return out


def search_functions(m, sc, patterns):
    """Funzioni il cui nome contiene i testi indicati: elenco, chi le usa e schema delle principali"""
    out = ["RICERCA DI FUNZIONI: %s" % ", ".join(patterns), "ID build: %s" % m.build_id, ""]
    selected = {}
    for pattern in patterns:
        found = sorted(m.find_functions(pattern), key=lambda f: (len(f[2]), f[2]))
        selected[pattern] = found
    watched = {f[0]: f[2] for found in selected.values() for f in found[:12]}

    print("Scansione del codice (puo' richiedere un paio di minuti)...", file=sys.stderr)
    users = {}

    def on_ref(pc, addr):
        if addr in watched:
            users.setdefault(watched[addr], set()).add("(indirizzo) " + (m.function_of(pc) or "?"))

    def on_call(pc, target):
        if target in watched:
            users.setdefault(watched[target], set()).add(m.function_of(pc) or "?")

    sc.scan(m.text[0], m.text[1] & ~3, on_ref, on_call)

    for pattern in patterns:
        found = selected[pattern]
        out.append("=== '%s': %d funzioni ===" % (pattern, len(found)))
        for start, end, name in found[:150]:
            out.append("  " + name)
        out.append("")
        for start, end, name in found[:6]:
            out.append("%s (%d byte)" % (name, end - start))
            callers = sorted(users.get(name, []))
            out.append("   usata da: " + (", ".join(callers[:30]) or "nessuno (o tramite tabelle virtuali)"))
            if end - start <= 6000:
                out.extend(outline(m, sc, start, end))
            out.append("")
    return out


CONFIG_WORDS = ["MAP", "filename", "checkpoint", "INIT", "debugGameMode", "PRINT_CONTROL", "PrintToFile",
                "ram:/alchemy.xml", "alchemy.xml", "build", "version", "debug"]
CONFIG_PARTS = ("alchemy", "registry", ".xml", ".ini", ".cfg", "app:/", "rom:/", "ram:/", "host:/", "config")
CONFIG_NAMES = ("Config", "Registry", "Alchemy", "alchemy", "Ini", "Settings")
CONFIG_VERBS = ("load", "read", "parse", "init", "open", "file", "instance", "create", "get", "set", "find")


def config_report(m, sc):
    """Da dove viene la configurazione letta da loadStartupMap (sezione MAP, chiave filename)"""
    out = ["CONFIGURAZIONE DEL GIOCO (solo nomi e testi)", "ID build: %s" % m.build_id, ""]
    fn = lambda pc: m.function_of(pc) or "?"  # noqa: E731

    # testi da cercare: parole esatte e testi che contengono certe parti
    targets = {}
    for word in CONFIG_WORDS:
        w = word.encode() + b"\0"
        k = m.ro[0]
        while True:
            k = m.mem.find(w, k, m.data[1])
            if k < 0:
                break
            targets[k] = word
            k += 1
    extra = []
    for mt in re.finditer(rb"[\x20-\x7e]{3,200}\0", bytes(m.mem[m.ro[0]:m.data[1]])):
        text = mt.group()[:-1].decode("ascii")
        low = text.lower()
        if any(part in low for part in CONFIG_PARTS) and not text.startswith("_Z"):
            addr = m.ro[0] + mt.start()
            if addr not in targets and len(extra) < 600:
                targets[addr] = text
                extra.append(text)

    roots = [f for f in m.funcs if "TheConfig" in f[2] or "TheFinalConfig" in f[2]][:12]
    root_set = {f[0]: f[2] for f in roots}
    root_globals = {}
    for start, end, name in roots:
        def on_root_ref(pc, addr, name=name):
            if m.is_data(addr):
                root_globals.setdefault(addr, set()).add(name)
        sc.scan(start, end, on_root_ref)

    print("Scansione del codice (puo' richiedere un paio di minuti)...", file=sys.stderr)
    users, gusers, callers = {}, {}, {}

    def on_ref(pc, addr):
        if addr in targets:
            users.setdefault(targets[addr], set()).add(fn(pc))
        elif addr in m.relative and m.relative[addr] in targets:
            users.setdefault(targets[m.relative[addr]], set()).add("(tabella) " + fn(pc))
        if addr in root_globals:
            gusers.setdefault(addr, set()).add(fn(pc))

    def on_call(pc, target):
        if target in root_set:
            callers.setdefault(root_set[target], set()).add(fn(pc))

    sc.scan(m.text[0], m.text[1] & ~3, on_ref, on_call)

    vtables = set()
    shown = set()

    def show(start, end, name, title=None):
        if name in shown:
            return
        shown.add(name)
        out.append("")
        out.append(title or "%s (%d byte)" % (name, end - start))
        if end - start <= 12000:
            out.extend(outline(m, sc, start, end, vtables=vtables))
        else:
            out.append("   (troppo lunga, non riportata)")

    out.append("=== TheConfig / TheFinalConfig ===")
    if not roots:
        out.append("nessuna funzione trovata")
    for start, end, name in roots:
        out.append("%s: chiamata da %d funzioni" % (name, len(callers.get(name, ()))))
    for start, end, name in roots:
        show(start, end, name)
    out.append("")
    out.append("--- dati globali usati da queste funzioni e chi altro li usa ---")
    others = []
    for addr in sorted(root_globals):
        who = sorted(gusers.get(addr, set()) - set(root_set.values()))
        out.append("%s (usato da %s): %s" % (_name_of_data(m, addr), ", ".join(sorted(root_globals[addr])),
                                             ", ".join(who[:30]) or "nessun altro"))
        others.extend(who)
    for name in sorted(set(others), key=lambda n: (len(n), n))[:10]:
        for start, end, n in m.funcs:
            if n == name:
                show(start, end, n)
                break

    out.append("")
    out.append("=== TESTI: chi li usa ===")
    for word in CONFIG_WORDS:
        who = sorted(users.get(word, ()))
        out.append("'%s': %s" % (word, ", ".join(who[:25]) + (" ... (%d)" % len(who) if len(who) > 25 else "")
                                 if who else "nessuno"))
    for text in extra:
        who = sorted(users.get(text, ()))
        if who:
            out.append("'%s': %s" % (text, ", ".join(who[:12]) + (" ... (%d)" % len(who) if len(who) > 12 else "")))

    out.append("")
    out.append("=== FUNZIONI CHE USANO I FILE DI CONFIGURAZIONE ===")
    file_users = set()
    for text in ["ram:/alchemy.xml", "alchemy.xml"] + [t for t in extra if "alchemy" in t.lower() or
                                                       t.lower().endswith((".xml", ".ini", ".cfg"))]:
        file_users.update(u for u in users.get(text, ()) if not u.startswith("(tabella)"))
    for name in sorted(file_users, key=lambda n: (len(n), n))[:12]:
        for start, end, n in m.funcs:
            if n == name:
                show(start, end, n)
                break

    out.append("")
    out.append("=== loadStartupMap: parte che legge la configurazione ===")
    for start, end, name in m.find_functions("loadStartupMap")[:1]:
        out.append(name)
        out.extend(outline(m, sc, start, end, ranges=[(0, 0x140), (0x480, 0x500), (0x6C0, 0x700)], vtables=vtables))

    out.append("")
    out.append("=== TABELLE VIRTUALI USATE (offset del metodo -> funzione) ===")
    for addr in sorted(vtables)[:20]:
        out.append("%s" % _name_of_data(m, addr))
        out.extend(vtable_entries(m, addr))

    out.append("")
    out.append("=== NOMI DI FUNZIONI (configurazione e registro) ===")
    names = [f[2] for f in m.funcs if any(c in f[2] for c in CONFIG_NAMES)
             and any(v in f[2].lower() for v in CONFIG_VERBS)]
    names = sorted(set(names), key=lambda n: (len(n), n))
    for name in names[:400]:
        out.append("  " + name)
    if len(names) > 400:
        out.append("  ... altri %d" % (len(names) - 400))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    patterns = sys.argv[2:]
    if os.path.isdir(path):
        candidates = []
        for root, _, files in os.walk(path):
            for name in files:
                if name.lower() in ("main", "main.nso") or name.lower().startswith("main-"):
                    candidates.append(os.path.join(root, name))
        if not candidates:
            sys.exit("File main non trovato in %s" % path)
        candidates.sort(key=lambda p: (os.path.basename(p).lower() != "main", p))
        path = candidates[0]

    with open(path, "rb") as f:
        m = Module(f.read())
    sc = Scanner(m)

    if patterns == ["--config"]:
        with open("configurazione.txt", "w", encoding="utf-8") as f:
            f.write("\n".join(config_report(m, sc)) + "\n")
        print("Scritto configurazione.txt: allegalo in chat.")
        return
    if patterns:
        with open("funzioni.txt", "w", encoding="utf-8") as f:
            f.write("\n".join(search_functions(m, sc, patterns)) + "\n")
        print("Scritto funzioni.txt: allegalo in chat.")
        return
    out = ["ANALISI DELLE OPZIONI DI AVVIO (solo nomi e testi)", "file: %s" % os.path.basename(path),
           "ID build: %s" % m.build_id, "simboli: %d, funzioni: %d, importazioni: %d" % (
               len(m.syms), len(m.funcs), len(m.got)), ""]

    # 1) indirizzi dei testi che interessano
    targets = {}
    for word in (b"-om", b"om", b"loadmap %s"):
        start = 0
        while True:
            k = m.mem.find(b"\0" + word + b"\0", start)
            if k < 0:
                break
            addr = k + 1
            if m.ro[0] <= addr < m.ro[1] or m.data[0] <= addr < m.data[1]:
                targets[addr] = word.decode()
            start = k + 1

    # 2) una sola scansione di tutto il codice
    print("Scansione del codice (puo' richiedere un paio di minuti)...", file=sys.stderr)
    arg_callers, string_users, table_users = {}, {}, {}
    option_slots = {slot: addr for slot, addr in m.relative.items() if addr in targets}

    def on_ref(pc, addr):
        if addr in targets:
            string_users.setdefault(targets[addr], set()).add(m.function_of(pc) or "?")
        for slot in option_slots:
            if 0 <= slot - addr < 0x400:
                table_users.setdefault(slot, set()).add(m.function_of(pc) or "?")

    def on_call(pc, target):
        name = sc.call_name(target)
        if name and ("GetHostArg" in name or "parseCommandLineArgs" in name or "igCommandLine5parse" in name
                     or "igCommandLine9parseFile" in name or "loadExtraArguments" in name
                     or "loadStartupMap" in name or "gameMainInitialize" in name):
            arg_callers.setdefault(name, set()).add(m.function_of(pc) or "?")

    sc.scan(m.text[0], m.text[1] & ~3, on_ref, on_call)

    out.append("=== CHI LEGGE GLI ARGOMENTI ===")
    for name in sorted(arg_callers):
        out.append("%s chiamata da:" % name)
        for caller in sorted(arg_callers[name]):
            out.append("    " + caller)
    if not arg_callers:
        out.append("nessuna chiamata trovata")
    out.append("")

    out.append("=== CHI USA I TESTI ===")
    for word in ("-om", "om", "loadmap %s"):
        users = string_users.get(word, set())
        out.append("'%s' (%d copie) usato da: %s" % (word, sum(1 for t in targets.values() if t == word),
                                                    ", ".join(sorted(users)) or "nessuno"))
    out.append("")

    out.append("=== TABELLA DELLE OPZIONI (voci vicine a '-om') ===")
    for slot in sorted(option_slots):
        out.append("voce a 0x%X (%s), usata da: %s" % (slot, targets[option_slots[slot]],
                                                    ", ".join(sorted(table_users.get(slot, []))) or "?"))
        for d in range(-6, 10):
            a = slot + 8 * d
            if a in m.relative:
                tgt = m.relative[a]
                s = m.cstr(tgt)
                desc = repr(s) if s else (m.function_of(tgt) or "0x%X" % tgt)
            elif m.data[0] <= a < m.data[1] or m.ro[0] <= a < m.ro[1]:
                desc = "valore %d" % m.u64(a)
            else:
                continue
            out.append("   %+3d: %s" % (d, desc))
    if not option_slots:
        out.append("nessuna voce trovata")
    out.append("")

    out.append("=== FUNZIONI DI AVVIO: testi usati e funzioni chiamate ===")
    for part in KEY_FUNCTIONS:
        for start, end, name in m.find_functions(part)[:4]:
            strings, calls = describe_function(m, sc, start, end)
            out.append("%s (%d byte)" % (name, end - start))
            out.append("   testi: " + (" | ".join(strings[:40]) or "-"))
            out.append("   chiama: " + (" | ".join(calls[:60]) or "-"))
    out.append("")

    out.append("=== SCHEMA DELLE FUNZIONI DI AVVIO (solo chiamate, testi, confronti e salti) ===")
    for part in OUTLINE_FUNCTIONS:
        for start, end, name in m.find_functions(part)[:2]:
            out.append("")
            out.append("%s (%d byte)" % (name, end - start))
            out.extend(outline(m, sc, start, end))
    out.append("")

    with open("avvio.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("Scritto avvio.txt: allegalo in chat.")


if __name__ == "__main__":
    main()
