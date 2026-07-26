#!/usr/bin/env python3
"""Link freestanding x86-64 objects into an ET_EXEC guest *with a symbol table*.

`source/translator/linux/tests/mklinuxelf.py` is the in-tree stand-in for a
missing ELF linker, but it writes program headers only (`e_shnum == 0`), which
is exactly the shape AOT cannot consume: discovery needs `.symtab`. This is the
same idea with a section table, a `.symtab` and a `.strtab`, so the AOT
pipeline can be exercised on a small guest instead of only on the 800 KB
glibc corpus.

Scope is deliberately identical to mklinuxelf.py: `-ffreestanding -nostdlib
-fno-pic -mcmodel=small` single-TU objects, R_X86_64_{64,PC32,PLT32,32,32S,PC64}.
Anything else raises.

    python3 mkaotguest.py -o guest.elf obj.o [--base 0x400000] [--entry _start]
"""

import argparse
import os
import struct

EM_X86_64, ET_EXEC, PT_LOAD = 62, 2, 1
PF_X, PF_W, PF_R = 1, 2, 4
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS = 1, 2, 3, 4, 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 0x1, 0x2, 0x4
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_FUNC, STT_OBJECT = 2, 1

EHDR_FMT, EHDR_SIZE = "<16sHHIQQQIHHHHHH", 64
PHDR_FMT, PHDR_SIZE = "<IIQQQQQQ", 56
SHDR_FMT, SHDR_SIZE = "<IIQQQQIIQQ", 64
SYM_FMT, SYM_SIZE = "<IBBHQQ", 24
RELA_FMT, RELA_SIZE = "<QQq", 24


def align_up(v, a):
    return (v + a - 1) & ~(a - 1) if a > 1 else v


class Obj:
    def __init__(self, path):
        self.path = path
        self.blob = open(path, "rb").read()
        (_, e_type, e_machine, _, _, _, e_shoff, _, _, _, _, _,
         e_shnum, e_shstrndx) = struct.unpack(EHDR_FMT, self.blob[:EHDR_SIZE])
        if e_machine != EM_X86_64:
            raise SystemExit(f"{path}: not x86-64")
        self.sections = []
        for i in range(e_shnum):
            o = e_shoff + i * SHDR_SIZE
            (nm, typ, flags, addr, off, size, link, info, align, entsize) = \
                struct.unpack(SHDR_FMT, self.blob[o:o + SHDR_SIZE])
            self.sections.append(dict(nm=nm, type=typ, flags=flags, offset=off,
                                      size=size, link=link, info=info,
                                      align=align, entsize=entsize, index=i))
        shstr = self.sections[e_shstrndx]
        names = self.blob[shstr["offset"]:shstr["offset"] + shstr["size"]]
        for s in self.sections:
            s["name"] = names[s["nm"]:names.find(b"\0", s["nm"])].decode()
        self.addr = {}
        symtab = next((s for s in self.sections if s["type"] == SHT_SYMTAB), None)
        self.symbols = []
        if symtab:
            strtab = self.sections[symtab["link"]]
            sd = self.blob[strtab["offset"]:strtab["offset"] + strtab["size"]]
            for i in range(symtab["size"] // SYM_SIZE):
                o = symtab["offset"] + i * SYM_SIZE
                (nmo, info, other, shndx, value, size) = \
                    struct.unpack(SYM_FMT, self.blob[o:o + SYM_SIZE])
                self.symbols.append(dict(
                    name=sd[nmo:sd.find(b"\0", nmo)].decode(), bind=info >> 4,
                    type=info & 0xF, other=other, shndx=shndx, value=value,
                    size=size, obj=self))

    def data(self, sec):
        if sec["type"] == SHT_NOBITS:
            return b"\0" * sec["size"]
        return self.blob[sec["offset"]:sec["offset"] + sec["size"]]


def resolve(sym):
    if sym["shndx"] == SHN_COMMON:
        return sym["common_addr"]
    if sym["shndx"] == SHN_ABS:
        return sym["value"]
    return sym["obj"].addr[sym["shndx"]] + sym["value"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("objects", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--base", default="0x400000")
    ap.add_argument("--entry", default="_start")
    args = ap.parse_args()
    base = int(args.base, 0)
    objs = [Obj(p) for p in args.objects]

    def rank(sec):
        if sec["type"] == SHT_NOBITS:
            return 3
        if sec["flags"] & SHF_EXECINSTR:
            return 0
        if sec["name"].startswith(".rodata"):
            return 1
        return 2

    chunks = []
    for obj in objs:
        for sec in obj.sections:
            if (sec["flags"] & SHF_ALLOC) and sec["type"] in (SHT_PROGBITS, SHT_NOBITS):
                chunks.append((rank(sec), obj, sec))
    chunks.sort(key=lambda c: c[0])

    # One PT_LOAD, file offset == vaddr - base, so the on-disk prefix is the
    # mapped image. Section headers/symtab go after it.
    cursor = base + EHDR_SIZE + PHDR_SIZE
    for _, obj, sec in chunks:
        cursor = align_up(cursor, max(sec["align"], 1))
        obj.addr[sec["index"]] = cursor
        cursor += sec["size"]
    commons = {}
    for obj in objs:
        for sym in obj.symbols:
            if sym["shndx"] != SHN_COMMON:
                continue
            if sym["name"] not in commons:
                cursor = align_up(cursor, max(sym["value"], 1))
                commons[sym["name"]] = cursor
                cursor += sym["size"]
            sym["common_addr"] = commons[sym["name"]]
    image_end = cursor

    globals_ = {}
    for obj in objs:
        for sym in obj.symbols:
            if sym["bind"] == STB_LOCAL or not sym["name"]:
                continue
            prev = globals_.get(sym["name"])
            if prev is None or prev["shndx"] == SHN_UNDEF:
                globals_[sym["name"]] = sym

    def lookup(obj, index):
        sym = obj.symbols[index]
        if sym["shndx"] == SHN_UNDEF and sym["name"]:
            r = globals_.get(sym["name"])
            if r is None or r["shndx"] == SHN_UNDEF:
                raise SystemExit(f"undefined symbol: {sym['name']}")
            return r
        return sym

    image = bytearray(image_end - base)
    for _, obj, sec in chunks:
        start = obj.addr[sec["index"]] - base
        image[start:start + sec["size"]] = obj.data(sec)

    handled = {1: 8, 2: 4, 4: 4, 10: 4, 11: 4, 24: 8}
    for obj in objs:
        for rela in obj.sections:
            if rela["type"] != SHT_RELA:
                continue
            target = obj.sections[rela["info"]]
            if target["index"] not in obj.addr:
                continue
            tbase = obj.addr[target["index"]]
            for i in range(rela["size"] // RELA_SIZE):
                o = rela["offset"] + i * RELA_SIZE
                r_off, r_info, r_add = struct.unpack(RELA_FMT, obj.blob[o:o + RELA_SIZE])
                r_type, r_sym = r_info & 0xFFFFFFFF, r_info >> 32
                if r_type not in handled:
                    raise SystemExit(f"unsupported relocation {r_type}")
                s = resolve(lookup(obj, r_sym))
                p = tbase + r_off
                value = s + r_add - p if r_type in (2, 4, 24) else s + r_add
                width = handled[r_type]
                if width == 4:
                    packed = struct.pack("<i", value if value < (1 << 31) else value - (1 << 32))
                else:
                    packed = struct.pack("<Q", value & ((1 << 64) - 1))
                image[p - base:p - base + width] = packed

    entry_sym = globals_.get(args.entry)
    if entry_sym is None:
        raise SystemExit(f"entry symbol {args.entry} not found")
    entry = resolve(entry_sym)

    last_progbits = base + EHDR_SIZE + PHDR_SIZE
    for _, obj, sec in chunks:
        if sec["type"] != SHT_NOBITS:
            last_progbits = max(last_progbits, obj.addr[sec["index"]] + sec["size"])
    filesz = last_progbits - base
    memsz = image_end - base

    # --- output sections --------------------------------------------------
    # One output section per input alloc section, in layout order, so the
    # guest looks like something a real linker produced.
    out_secs = [dict(name="", type=0, flags=0, addr=0, size=0, align=0,
                     link=0, info=0, entsize=0, data=b"")]
    sec_of_input = {}
    for _, obj, sec in chunks:
        a = obj.addr[sec["index"]]
        out_secs.append(dict(name=sec["name"], type=sec["type"], flags=sec["flags"],
                             addr=a, size=sec["size"], align=max(sec["align"], 1),
                             link=0, info=0, entsize=0,
                             data=b"" if sec["type"] == SHT_NOBITS
                             else bytes(image[a - base:a - base + sec["size"]]),
                             file_off=a - base))
        sec_of_input[(id(obj), sec["index"])] = len(out_secs) - 1

    # --- symbol table -----------------------------------------------------
    strtab = bytearray(b"\0")
    def add_str(s):
        off = len(strtab)
        strtab.extend(s.encode() + b"\0")
        return off

    syms = [struct.pack(SYM_FMT, 0, 0, 0, 0, 0, 0)]
    locals_first = []
    globals_last = []
    for obj in objs:
        for sym in obj.symbols:
            if not sym["name"] or sym["type"] not in (STT_FUNC, STT_OBJECT):
                continue
            if sym["shndx"] == SHN_COMMON:
                value, shndx = sym["common_addr"], len(out_secs) - 1
            elif sym["shndx"] in (SHN_UNDEF, SHN_ABS) or sym["shndx"] >= 0xFF00:
                continue
            else:
                key = (id(obj), sym["shndx"])
                if key not in sec_of_input:
                    continue
                value, shndx = resolve(sym), sec_of_input[key]
            rec = struct.pack(SYM_FMT, add_str(sym["name"]),
                              (sym["bind"] << 4) | sym["type"], 0, shndx,
                              value, sym["size"])
            (locals_first if sym["bind"] == STB_LOCAL else globals_last).append(rec)
    n_local = 1 + len(locals_first)
    syms.extend(locals_first)
    syms.extend(globals_last)

    symtab_idx = len(out_secs)
    out_secs.append(dict(name=".symtab", type=SHT_SYMTAB, flags=0, addr=0,
                         size=len(syms) * SYM_SIZE, align=8, link=symtab_idx + 1,
                         info=n_local, entsize=SYM_SIZE, data=b"".join(syms)))
    out_secs.append(dict(name=".strtab", type=SHT_STRTAB, flags=0, addr=0,
                         size=len(strtab), align=1, link=0, info=0, entsize=0,
                         data=bytes(strtab)))
    shstr = bytearray(b"\0")
    for s in out_secs:
        s["nm"] = 0 if not s["name"] else len(shstr)
        if s["name"]:
            shstr.extend(s["name"].encode() + b"\0")
    shstrndx = len(out_secs)
    out_secs.append(dict(name=".shstrtab", type=SHT_STRTAB, flags=0, addr=0,
                         size=0, align=1, link=0, info=0, entsize=0, data=b"", nm=len(shstr)))
    shstr.extend(b".shstrtab\0")
    out_secs[-1]["size"] = len(shstr)
    out_secs[-1]["data"] = bytes(shstr)

    # --- write ------------------------------------------------------------
    out = bytearray(image[:filesz])
    pos = len(out)
    for s in out_secs:
        if s["type"] in (0, SHT_NOBITS):
            s["off"] = s.get("file_off", 0)
            continue
        if "file_off" in s:
            s["off"] = s["file_off"]
            continue
        pos = align_up(pos, max(s["align"], 1))
        out.extend(b"\0" * (pos - len(out)))
        s["off"] = pos
        out.extend(s["data"])
        pos = len(out)
    pos = align_up(pos, 8)
    out.extend(b"\0" * (pos - len(out)))
    shoff = pos
    for s in out_secs:
        out.extend(struct.pack(SHDR_FMT, s["nm"], s["type"], s["flags"], s["addr"],
                               s["off"], s["size"], s["link"], s["info"],
                               s["align"], s["entsize"]))

    ehdr = struct.pack(EHDR_FMT, b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8),
                       ET_EXEC, EM_X86_64, 1, entry, EHDR_SIZE, shoff, 0,
                       EHDR_SIZE, PHDR_SIZE, 1, SHDR_SIZE, len(out_secs), shstrndx)
    phdr = struct.pack(PHDR_FMT, PT_LOAD, PF_R | PF_W | PF_X, 0, base, base,
                       filesz, memsz, 0x1000)
    out[0:EHDR_SIZE] = ehdr
    out[EHDR_SIZE:EHDR_SIZE + PHDR_SIZE] = phdr
    with open(args.out, "wb") as h:
        h.write(out)
    os.chmod(args.out, 0o755)
    n_func = sum(1 for r in syms[1:]
                 if struct.unpack(SYM_FMT, r)[1] & 0xF == STT_FUNC)
    print(f"{args.out}: entry {entry:#x} filesz {filesz} memsz {memsz} "
          f"sections {len(out_secs)} symbols {len(syms)} STT_FUNC {n_func}")


if __name__ == "__main__":
    main()
