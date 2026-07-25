#!/usr/bin/env python3
"""Build clone_smc_mt_x86_64 as a minimal static RWX ELF on macOS."""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "clone_smc_mt_x86_64.S")
OBJ = os.path.join(HERE, "clone_smc_mt_x86_64.o")
RAW = os.path.join(HERE, "clone_smc_mt_x86_64.bin")
OUT = os.path.join(HERE, "clone_smc_mt_x86_64")

VADDR = 0x400000
PAGE = 0x1000
CLANG = os.environ.get("CLANG", "clang")
OBJCOPY = os.environ.get("LLVM_OBJCOPY", "/opt/homebrew/opt/llvm/bin/llvm-objcopy")


def main() -> int:
    subprocess.check_call([CLANG, "--target=x86_64-linux-gnu", "-c", SRC, "-o", OBJ])
    subprocess.check_call([OBJCOPY, "-O", "binary", "--only-section=.smc", OBJ, RAW])
    with open(RAW, "rb") as source:
        image = source.read()

    ehdr_size = 64
    phdr_size = 56
    image_offset = ehdr_size + phdr_size
    entry = VADDR + image_offset
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH",
        ident,
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
    file_size = image_offset + len(image)
    phdr = struct.pack(
        "<IIQQQQQQ",
        1,
        7,  # PF_R | PF_W | PF_X
        0,
        VADDR,
        VADDR,
        file_size,
        file_size,
        PAGE,
    )
    with open(OUT, "wb") as output:
        output.write(ehdr)
        output.write(phdr)
        output.write(image)
    os.chmod(OUT, 0o755)
    print(f"wrote {OUT}: entry {entry:#x}, {file_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())

