#!/usr/bin/env python3
"""Link freestanding x86-64 ELF relocatable objects into a static ET_EXEC ELF.

WHY THIS EXISTS.  The canonical way to rebuild the guest binaries under this
directory is `build_real_tests.sh` inside a Linux machine (see its header).
That is not always available: an Apple-Silicon workstation with only the Xcode
command line tools has a clang that happily *compiles* for
`x86_64-unknown-linux-gnu` but ships no ELF linker at all.  This script is that
missing linker, and nothing more.

It is deliberately tiny and only handles what `-ffreestanding -nostdlib
-fno-pic` single-translation-unit guests actually produce:

  * SHF_ALLOC SHT_PROGBITS / SHT_NOBITS sections, laid out .text -> .rodata ->
    .data -> .bss and covered by one RWX PT_LOAD,
  * .symtab / .strtab symbol resolution (locals per object, globals shared,
    SHN_COMMON promoted into .bss),
  * the small-code-model relocations clang emits for that mode:
    R_X86_64_64, _PC32, _PLT32, _32, _32S, _PC64.

Anything else raises.  If you need more you are probably better off using a
real linker on a real Linux box; see build_real_tests.sh.

Usage:
    python3 mklinuxelf.py -o guest.elf obj1.o [obj2.o ...]
        [--base 0x400000] [--entry _start]
"""

import argparse
import os
import struct

EM_X86_64 = 62
ET_EXEC = 2
PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4

SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS = 1, 2, 3, 4, 8
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4

SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2

STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_SECTION = 3

EHDR_FMT = "<16sHHIQQQIHHHHHH"
EHDR_SIZE = 64
PHDR_FMT = "<IIQQQQQQ"
PHDR_SIZE = 56
SHDR_FMT = "<IIQQQQIIQQ"
SHDR_SIZE = 64
SYM_FMT = "<IBBHQQ"
SYM_SIZE = 24
RELA_FMT = "<QQq"
RELA_SIZE = 24


def align_up(value, alignment):
    if alignment <= 1:
        return value
    return (value + alignment - 1) & ~(alignment - 1)


class Obj:
    """One parsed ELF64 relocatable input file."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as handle:
            self.blob = handle.read()
        ident = self.blob[:16]
        if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
            raise SystemExit("%s: not a little-endian ELF64 file" % path)
        fields = struct.unpack(EHDR_FMT, self.blob[:EHDR_SIZE])
        (_, e_type, e_machine, _, _, _, e_shoff, _, _, _, _, _, e_shnum,
         e_shstrndx) = fields
        if e_type != 1 or e_machine != EM_X86_64:
            raise SystemExit("%s: expected an x86-64 relocatable object" % path)
        self.sections = []
        for i in range(e_shnum):
            off = e_shoff + i * SHDR_SIZE
            (name, stype, flags, addr, offset, size, link, info, addralign,
             entsize) = struct.unpack(SHDR_FMT, self.blob[off:off + SHDR_SIZE])
            self.sections.append({
                "name_off": name, "type": stype, "flags": flags, "addr": addr,
                "offset": offset, "size": size, "link": link, "info": info,
                "align": addralign, "entsize": entsize, "index": i,
            })
        shstr = self.sections[e_shstrndx]
        strtab = self.blob[shstr["offset"]:shstr["offset"] + shstr["size"]]
        for sec in self.sections:
            end = strtab.find(b"\0", sec["name_off"])
            sec["name"] = strtab[sec["name_off"]:end].decode()
        # Output address of each allocated section, filled in by the layout pass.
        self.addr = {}
        self.symbols = self._read_symbols()

    def data(self, sec):
        if sec["type"] == SHT_NOBITS:
            return b"\0" * sec["size"]
        return self.blob[sec["offset"]:sec["offset"] + sec["size"]]

    def _read_symbols(self):
        symtab = next((s for s in self.sections if s["type"] == SHT_SYMTAB), None)
        if symtab is None:
            return []
        strtab = self.sections[symtab["link"]]
        strdata = self.blob[strtab["offset"]:strtab["offset"] + strtab["size"]]
        out = []
        for i in range(symtab["size"] // SYM_SIZE):
            off = symtab["offset"] + i * SYM_SIZE
            name_off, info, other, shndx, value, size = struct.unpack(
                SYM_FMT, self.blob[off:off + SYM_SIZE])
            end = strdata.find(b"\0", name_off)
            out.append({
                "name": strdata[name_off:end].decode(), "bind": info >> 4,
                "type": info & 0xF, "other": other, "shndx": shndx,
                "value": value, "size": size, "obj": self,
            })
        return out


def resolve(sym):
    """Final absolute address of a (defined) symbol."""
    if sym["shndx"] == SHN_ABS:
        return sym["value"]
    if sym["shndx"] == SHN_COMMON:
        return sym["common_addr"]
    if sym["shndx"] == SHN_UNDEF:
        if sym["bind"] == STB_WEAK:
            return 0
        raise SystemExit("undefined symbol: %s (in %s)"
                         % (sym["name"], sym["obj"].path))
    base = sym["obj"].addr.get(sym["shndx"])
    if base is None:
        raise SystemExit("symbol %s lives in a non-allocated section"
                         % sym["name"])
    return base + sym["value"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("objects", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--base", default="0x400000")
    ap.add_argument("--entry", default="_start")
    args = ap.parse_args()

    base = int(args.base, 0)
    objs = [Obj(p) for p in args.objects]

    # --- layout -----------------------------------------------------------
    # Everything lands in one RWX PT_LOAD whose file offset equals
    # (vaddr - base), so the on-disk image is a byte-for-byte picture of the
    # mapped image.  .bss (SHT_NOBITS) goes last so filesz < memsz.
    def rank(sec):
        name = sec["name"]
        if sec["type"] == SHT_NOBITS:
            return 3
        if sec["flags"] & SHF_EXECINSTR:
            return 0
        if name.startswith(".rodata"):
            return 1
        return 2

    chunks = []
    for obj in objs:
        for sec in obj.sections:
            if not (sec["flags"] & SHF_ALLOC):
                continue
            if sec["type"] not in (SHT_PROGBITS, SHT_NOBITS):
                continue
            chunks.append((rank(sec), obj, sec))
    chunks.sort(key=lambda c: c[0])

    cursor = base + EHDR_SIZE + PHDR_SIZE
    for _, obj, sec in chunks:
        cursor = align_up(cursor, max(sec["align"], 1))
        obj.addr[sec["index"]] = cursor
        cursor += sec["size"]

    # SHN_COMMON symbols have no input section; give each one storage after
    # the last real section (still inside the single PT_LOAD, before .bss end).
    commons = {}
    for obj in objs:
        for sym in obj.symbols:
            if sym["shndx"] != SHN_COMMON:
                continue
            key = sym["name"]
            if key not in commons:
                cursor = align_up(cursor, max(sym["value"], 1))
                commons[key] = (cursor, sym["size"])
                cursor += sym["size"]
            sym["common_addr"] = commons[key][0]
    image_end = cursor

    # --- symbol table -----------------------------------------------------
    globals_ = {}
    for obj in objs:
        for sym in obj.symbols:
            if sym["bind"] == STB_LOCAL or not sym["name"]:
                continue
            if sym["shndx"] == SHN_UNDEF:
                globals_.setdefault(sym["name"], sym)
                continue
            prev = globals_.get(sym["name"])
            if prev is not None and prev["shndx"] != SHN_UNDEF and \
                    prev["bind"] != STB_WEAK and sym["bind"] != STB_WEAK:
                raise SystemExit("duplicate symbol: %s" % sym["name"])
            if prev is None or prev["shndx"] == SHN_UNDEF or \
                    prev["bind"] == STB_WEAK:
                globals_[sym["name"]] = sym

    def lookup(obj, index):
        sym = obj.symbols[index]
        if sym["shndx"] == SHN_UNDEF and sym["name"]:
            resolved = globals_.get(sym["name"])
            if resolved is None or resolved["shndx"] == SHN_UNDEF:
                if sym["bind"] == STB_WEAK:
                    return None
                raise SystemExit("undefined symbol: %s (referenced by %s)"
                                 % (sym["name"], obj.path))
            return resolved
        return sym

    # --- emit section bytes, then relocate --------------------------------
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
                continue  # relocations against a non-allocated section (debug)
            target_base = obj.addr[target["index"]]
            for i in range(rela["size"] // RELA_SIZE):
                off = rela["offset"] + i * RELA_SIZE
                r_off, r_info, r_add = struct.unpack(
                    RELA_FMT, obj.blob[off:off + RELA_SIZE])
                r_type, r_sym = r_info & 0xFFFFFFFF, r_info >> 32
                if r_type not in handled:
                    raise SystemExit(
                        "%s: unsupported relocation type %d against %s "
                        "(rebuild with -fno-pic -mcmodel=small, or link on Linux)"
                        % (obj.path, r_type, obj.symbols[r_sym]["name"]))
                sym = lookup(obj, r_sym)
                s = 0 if sym is None else resolve(sym)
                p = target_base + r_off
                if r_type in (2, 4, 24):
                    value = s + r_add - p
                else:
                    value = s + r_add
                width = handled[r_type]
                if width == 4:
                    if r_type == 10:
                        if not 0 <= value < (1 << 32):
                            raise SystemExit("R_X86_64_32 overflow")
                    elif not -(1 << 31) <= value < (1 << 31):
                        raise SystemExit("32-bit relocation overflow at %#x"
                                         % p)
                    packed = struct.pack("<i", value if value < (1 << 31)
                                         else value - (1 << 32))
                else:
                    packed = struct.pack("<Q", value & ((1 << 64) - 1))
                pos = p - base
                image[pos:pos + width] = packed

    # --- entry ------------------------------------------------------------
    entry_sym = globals_.get(args.entry)
    if entry_sym is None:
        for obj in objs:
            for sym in obj.symbols:
                if sym["name"] == args.entry and sym["shndx"] != SHN_UNDEF:
                    entry_sym = sym
    if entry_sym is None:
        raise SystemExit("entry symbol %s not found" % args.entry)
    entry = resolve(entry_sym)

    # --- write out --------------------------------------------------------
    # filesz stops at the last PROGBITS byte; memsz covers .bss and commons.
    last_progbits = base + EHDR_SIZE + PHDR_SIZE
    for _, obj, sec in chunks:
        if sec["type"] != SHT_NOBITS:
            last_progbits = max(last_progbits,
                                obj.addr[sec["index"]] + sec["size"])
    filesz = last_progbits - base
    memsz = image_end - base

    ehdr = struct.pack(EHDR_FMT, b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8),
                       ET_EXEC, EM_X86_64, 1, entry, EHDR_SIZE, 0, 0,
                       EHDR_SIZE, PHDR_SIZE, 1, SHDR_SIZE, 0, 0)
    phdr = struct.pack(PHDR_FMT, PT_LOAD, PF_R | PF_W | PF_X, 0, base, base,
                       filesz, memsz, 0x1000)
    out = bytearray(image[:filesz])
    out[0:EHDR_SIZE] = ehdr
    out[EHDR_SIZE:EHDR_SIZE + PHDR_SIZE] = phdr
    with open(args.out, "wb") as handle:
        handle.write(out)
    os.chmod(args.out, 0o755)
    print("%s: entry %#x, filesz %d, memsz %d" % (args.out, entry, filesz, memsz))


if __name__ == "__main__":
    main()
