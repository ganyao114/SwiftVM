#!/usr/bin/env python3
"""Build the minimal static rdtsc_monotonic_x86_64 Linux ELF fixture."""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STEM = sys.argv[1] if len(sys.argv) > 1 else "rdtsc_monotonic_x86_64"
SRC = os.path.join(HERE, STEM + ".S")
OBJ = os.path.join(HERE, STEM + ".o")
BIN = os.path.join(HERE, STEM + ".bin")
OUT = os.path.join(HERE, STEM)
VADDR = 0x400000


def main():
    clang = os.environ.get("CLANG", "clang")
    objcopy = os.environ.get(
        "LLVM_OBJCOPY", "/opt/homebrew/opt/llvm/bin/llvm-objcopy"
    )
    subprocess.check_call([clang, "--target=x86_64-linux-gnu", "-c", SRC, "-o", OBJ])
    subprocess.check_call([objcopy, "-O", "binary", "--only-section=.text", OBJ, BIN])
    with open(BIN, "rb") as source:
        code = source.read()

    ehdr_size = 64
    phdr_size = 56
    code_off = ehdr_size + phdr_size
    entry = VADDR + code_off
    e_ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH",
        e_ident,
        2,
        62,
        1,
        entry,
        ehdr_size,
        0,
        0,
        ehdr_size,
        phdr_size,
        1,
        0,
        0,
        0,
    )
    filesz = code_off + len(code)
    phdr = struct.pack(
        "<IIQQQQQQ", 1, 5, 0, VADDR, VADDR, filesz, filesz, 0x1000
    )
    with open(OUT, "wb") as target:
        target.write(ehdr)
        target.write(phdr)
        target.write(code)
    os.chmod(OUT, 0o755)
    os.remove(OBJ)
    os.remove(BIN)
    print(f"wrote {OUT}: entry {entry:#x}, {filesz} bytes")


if __name__ == "__main__":
    main()
