#!/usr/bin/env python3
"""Build smc_mt_stress_x86_64 as a minimal static RWX ELF (no objcopy needed)."""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.environ.get("SRC", os.path.join(HERE, "smc_mt_stress_x86_64.S"))
OBJ = SRC[:-2] + ".o"
OUT = os.environ.get("OUT", SRC[:-2])
VADDR = 0x400000
PAGE = 0x1000
CLANG = os.environ.get("CLANG", "clang")


def read_section(path, want):
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 2, "expect ELF64"
    (e_shoff,) = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    def sh(i):
        off = e_shoff + i * e_shentsize
        return struct.unpack_from("<IIQQQQIIQQ", data, off)
    _, _, _, _, str_off, str_size, *_ = sh(e_shstrndx)
    strtab = data[str_off:str_off + str_size]
    relocs = 0
    found = None
    for i in range(e_shnum):
        name_off, sh_type, _, _, off, size, _, _, _, _ = sh(i)
        name = strtab[name_off:strtab.index(b"\0", name_off)].decode()
        if name == want:
            found = data[off:off + size]
        if name in (".rela" + want, ".rel" + want):
            relocs = size
    assert found is not None, "section %s not found" % want
    assert relocs == 0, "section %s has unresolved relocations (%d bytes)" % (want, relocs)
    return found


def main():
    subprocess.check_call([CLANG, "--target=x86_64-linux-gnu", "-c", SRC, "-o", OBJ])
    image = read_section(OBJ, ".smc")
    ehdr_size, phdr_size = 64, 56
    image_offset = ehdr_size + phdr_size
    entry = VADDR + image_offset
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    ehdr = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, entry,
                       ehdr_size, 0, 0, ehdr_size, phdr_size, 1, 0, 0, 0)
    file_size = image_offset + len(image)
    phdr = struct.pack("<IIQQQQQQ", 1, 7, 0, VADDR, VADDR, file_size, file_size, PAGE)
    with open(OUT, "wb") as f:
        f.write(ehdr)
        f.write(phdr)
        f.write(image)
    os.chmod(OUT, 0o755)
    print("wrote %s: entry %#x, %d bytes" % (OUT, entry, file_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
