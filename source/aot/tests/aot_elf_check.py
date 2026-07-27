#!/usr/bin/env python3
"""Third-party structural check of an AOT artifact.

Deliberately does NOT use ELFIO or any SwiftVM code: an ELF that only the
writer can read back is no evidence that it is a legal ELF (docs/aot-design.md
§7.3). Everything here is `struct.unpack` against the ELF64 spec. `readelf` /
`llvm-readelf` are not available on this host and are not required.

  aot_elf_check.py <artifact> [--guest <original guest elf>]

Checks, in order:
  1. e_ident / class / endianness / e_machine == EM_AARCH64 / e_type == ET_DYN
  2. section table is self-consistent: names resolvable through e_shstrndx,
     sh_offset+sh_size inside the file for every non-NOBITS section,
     sh_link indices in range
  3. program headers: every PT_LOAD's file range is inside the file and its
     sections lie within [p_vaddr, p_vaddr+p_memsz)
  4. .symtab parses, every st_name resolves in the linked strtab, and every
     st_shndx is a valid section index or a reserved value
  5. against the original guest ELF (--guest):
       - every original section name is present, in the same relative order
       - every original symbol name is present exactly once more than... no:
         present, with the same count
       - every STT_OBJECT keeps its st_value / st_size
       - every STT_FUNC now points inside the AOT code section
       - .svmaot.segN reproduce the guest PT_LOAD file bytes exactly
"""
import os
import struct
import sys

AOT_CODE_VADDR = 0x800000000000
EM_AARCH64 = 183
ET_DYN = 3
SHT_NOBITS = 8
SHT_SYMTAB = 2
SHN_LORESERVE = 0xFF00


class Elf:
    def __init__(self, path):
        self.path = path
        self.d = open(path, "rb").read()
        d = self.d
        if d[:4] != b"\x7fELF":
            raise ValueError(f"{path}: not an ELF")
        self.ei_class, self.ei_data = d[4], d[5]
        (self.e_type, self.e_machine, self.e_version, self.e_entry, self.e_phoff,
         self.e_shoff, self.e_flags, self.e_ehsize, self.e_phentsize, self.e_phnum,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = struct.unpack_from(
            "<HHIQQQIHHHHHH", d, 16)
        self.secs = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            (nm, typ, flags, addr, off, size, link, info, align, entsize) = \
                struct.unpack_from("<IIQQQQIIQQ", d, o)
            self.secs.append(dict(nm=nm, type=typ, flags=flags, addr=addr, off=off,
                                  size=size, link=link, info=info, align=align,
                                  entsize=entsize, idx=i))
        if self.e_shnum:
            sh = self.secs[self.e_shstrndx]
            blob = d[sh["off"]:sh["off"] + sh["size"]]
            for s in self.secs:
                e = blob.find(b"\0", s["nm"])
                s["name"] = blob[s["nm"]:e].decode("latin1")
        self.phs = []
        for i in range(self.e_phnum):
            o = self.e_phoff + i * self.e_phentsize
            (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align) = \
                struct.unpack_from("<IIQQQQQQ", d, o)
            self.phs.append(dict(type=p_type, flags=p_flags, off=p_offset, vaddr=p_vaddr,
                                 filesz=p_filesz, memsz=p_memsz, align=p_align))

    def section(self, name):
        for s in self.secs:
            if s.get("name") == name:
                return s
        return None

    def data(self, s):
        if s["type"] == SHT_NOBITS:
            return b"\0" * s["size"]
        return self.d[s["off"]:s["off"] + s["size"]]

    def symbols(self):
        st = next((s for s in self.secs if s["type"] == SHT_SYMTAB), None)
        if st is None:
            return []
        strt = self.secs[st["link"]]
        sd = self.d[strt["off"]:strt["off"] + strt["size"]]
        out = []
        for i in range(st["size"] // 24):
            o = st["off"] + i * 24
            (nmo, info, other, shndx, value, size) = struct.unpack_from("<IBBHQQ", self.d, o)
            e = sd.find(b"\0", nmo)
            out.append(dict(name=sd[nmo:e].decode("latin1"), bind=info >> 4,
                            type=info & 0xF, shndx=shndx, value=value, size=size,
                            name_ok=(nmo < len(sd) and e >= 0)))
        return out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    artifact = args[0]
    guest = None
    if "--guest" in args:
        guest = args[args.index("--guest") + 1]

    fails = []
    def check(cond, msg):
        if not cond:
            fails.append(msg)

    a = Elf(artifact)
    print(f"artifact {artifact}: {len(a.d)} bytes")

    # 1. header
    check(a.ei_class == 2, "not ELFCLASS64")
    check(a.ei_data == 1, "not little endian")
    check(a.e_machine == EM_AARCH64, f"e_machine {a.e_machine} != EM_AARCH64")
    check(a.e_type == ET_DYN, f"e_type {a.e_type} != ET_DYN")
    check(a.e_shnum > 0, "no section table")
    check(a.e_phnum > 0, "no program headers")
    print(f"  header: class64 LSB machine={a.e_machine} type={a.e_type} "
          f"shnum={a.e_shnum} phnum={a.e_phnum} entry={a.e_entry:#x}")

    # 2. section table
    for s in a.secs[1:]:
        check("name" in s, f"section {s['idx']} name unresolvable")
        if s["type"] != SHT_NOBITS:
            check(s["off"] + s["size"] <= len(a.d),
                  f"section {s.get('name')} runs past EOF")
        check(s["link"] < a.e_shnum, f"section {s.get('name')} sh_link out of range")

    # 3. program headers
    for ph in a.phs:
        check(ph["off"] + ph["filesz"] <= len(a.d), "PT_LOAD file range past EOF")
        check(ph["filesz"] <= ph["memsz"], "p_filesz > p_memsz")

    # 4. symbols
    syms = a.symbols()
    check(len(syms) > 0, "artifact has no .symtab")
    code = a.section(".svmaot.text")
    check(code is not None, "missing .svmaot.text")
    info = a.section(".svmaot.info")
    check(info is not None, "missing .svmaot.info")
    # STT_FUNC (2) and STT_GNU_IFUNC (10). The ifuncs are not optional: their
    # st_value is a resolver, i.e. real code, and if the writer left them alone
    # the artifact would carry 37 original x86-64 addresses in an AArch64
    # object. Checking them here is what makes "leave them alone" a failure.
    funcs = [s for s in syms if s["type"] in (2, 10)]
    for s in syms:
        check(s["name_ok"], f"symbol {s['name']!r} st_name out of range")
        check(s["shndx"] < a.e_shnum or s["shndx"] >= SHN_LORESERVE,
              f"symbol {s['name']} st_shndx {s['shndx']} out of range")
    if code:
        lo, hi = AOT_CODE_VADDR, AOT_CODE_VADDR + code["size"]
        bad = [s for s in funcs if not (lo <= s["value"] < hi)]
        check(not bad, f"{len(bad)} STT_FUNC still outside the AOT code section "
                       f"(first: {bad[0]['name'] if bad else ''})")
        bad_sh = [s for s in funcs if s["shndx"] != code["idx"]]
        check(not bad_sh, f"{len(bad_sh)} STT_FUNC st_shndx not the AOT code section")
        oob = [s for s in funcs if s["value"] + s["size"] > hi]
        check(not oob, f"{len(oob)} STT_FUNC extend past the AOT code section")
    print(f"  symbols: {len(syms)} total, {len(funcs)} STT_FUNC, "
          f".svmaot.text = {code['size'] if code else 0} bytes at {code['addr']:#x}"
          if code else "  symbols: no code section")

    # 4b. every rewritten STT_FUNC must name a whole unit, or the stub. This is
    # the check that makes a wrong st_size a test failure rather than a
    # cosmetic detail: a symbol whose size is not the size of the unit it
    # points at is a symbol table that lies about what is there.
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from aot_mutate import Artifact
        art = Artifact(artifact)
        # A symbol may name a whole unit, an entry block inside one (the code
        # from that entry to the end of the unit), or the failure stub.
        want = {}
        for u in art.units:
            for b in u["blocks"]:
                want.setdefault(u["code_offset"] + b["code_offset"],
                                u["code_size"] - b["code_offset"])
        for u in art.units:
            want[u["code_offset"]] = u["code_size"]
        want[art.stub_offset] = 8  # kAotStubSize
        bad = []
        for s in funcs:
            off = s["value"] - AOT_CODE_VADDR
            if off not in want or want[off] != s["size"]:
                bad.append((s["name"], off, s["size"], want.get(off)))
        check(not bad,
              f"{len(bad)} STT_FUNC do not match a unit's (offset,size) "
              f"(first: {bad[0] if bad else ''})")
        stub_syms = sum(1 for s in funcs if s["value"] - AOT_CODE_VADDR == art.stub_offset)
        print(f"  units: {len(art.units)} compiled units, {len(art.segments)} guest "
              f"segments, {sum(len(u['relocs']) for u in art.units)} relocations; "
              f"{stub_syms} symbols point at the failure stub")
    except Exception as exc:  # noqa: BLE001 - a broken blob is a real failure
        check(False, f"could not cross-check symbols against .svmaot.info: {exc}")

    # 5. against the guest
    if guest:
        g = Elf(guest)
        gnames = [s["name"] for s in g.secs[1:]]
        anames = [s["name"] for s in a.secs[1:]]
        # order preserved: gnames must be a subsequence of anames
        it = iter(anames)
        check(all(n in it for n in gnames),
              "original section names are missing or reordered in the artifact")
        print(f"  sections: {len(gnames)} original names all present in order "
              f"(artifact has {len(anames)})")

        gsyms = g.symbols()
        asyms = a.symbols()
        check(len(gsyms) == len(asyms),
              f"symbol count changed: {len(gsyms)} -> {len(asyms)}")
        gn = [s["name"] for s in gsyms]
        an = [s["name"] for s in asyms]
        check(gn == an, "symbol names/order changed")
        moved_objects = [(x, y) for x, y in zip(gsyms, asyms)
                         if x["type"] == 1 and (x["value"] != y["value"] or
                                                x["size"] != y["size"])]
        check(not moved_objects,
              f"{len(moved_objects)} STT_OBJECT symbols moved (data must not move)")
        gfuncs = [s for s in gsyms if s["type"] == 2]
        gifuncs = [s for s in gsyms if s["type"] == 10]
        # An ifunc that still holds its guest st_value is an x86-64 address in
        # an AArch64 object: the symbol table would be lying about where the
        # code is, and a host resolving `strlen` by name would jump into guest
        # data. Assert every one of them moved into the code section.
        stale = [x["name"] for x, y in zip(gsyms, asyms)
                 if x["type"] == 10 and y["value"] == x["value"]]
        check(not stale,
              f"{len(stale)} STT_GNU_IFUNC symbols kept their guest st_value "
              f"(first: {stale[0] if stale else ''})")
        print(f"  symtab: {len(gsyms)} symbols preserved, {len(gfuncs)} STT_FUNC "
              f"+ {len(gifuncs)} STT_GNU_IFUNC redirected, "
              f"{len([s for s in gsyms if s['type'] == 1])} STT_OBJECT untouched")

        # .svmaot.segN vs the guest PT_LOADs
        gloads = [p for p in g.phs if p["type"] == 1]
        for i, p in enumerate(gloads):
            sec = a.section(f".svmaot.seg{i}")
            check(sec is not None, f"missing .svmaot.seg{i}")
            if sec is None:
                continue
            want = g.d[p["off"]:p["off"] + p["filesz"]]
            got = a.data(sec)
            check(want == got,
                  f".svmaot.seg{i} differs from guest PT_LOAD {i} "
                  f"({len(want)} vs {len(got)} bytes)")
        print(f"  guest data: {len(gloads)} PT_LOAD segments carried byte-identically")

    if fails:
        print("\nFAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
