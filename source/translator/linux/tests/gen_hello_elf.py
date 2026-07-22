#!/usr/bin/env python3
"""Build a minimal static ARM64 Linux ELF executable (hello_aarch64).

Assembles hello.S with clang, extracts the raw .text bytes with
llvm-objcopy, then wraps them in a hand-written ET_EXEC ELF (one PT_LOAD
segment at 0x400000). No cross binutils/lld required.
"""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "hello.S")
OBJ = os.path.join(HERE, "hello.o")
BIN = os.path.join(HERE, "hello.bin")
OUT = os.path.join(HERE, "hello_aarch64")

VADDR = 0x400000
PAGE = 0x1000

CLANG = os.environ.get("CLANG", "clang")
OBJCOPY = os.environ.get(
    "LLVM_OBJCOPY", "/opt/homebrew/opt/llvm/bin/llvm-objcopy"
)


def main():
    subprocess.check_call(
        [CLANG, "--target=aarch64-linux-gnu", "-c", SRC, "-o", OBJ]
    )
    subprocess.check_call([OBJCOPY, "-O", "binary", "--only-section=.text", OBJ, BIN])
    with open(BIN, "rb") as f:
        code = f.read()

    ehdr_size = 64
    phdr_size = 56
    code_off = ehdr_size + phdr_size  # code follows the phdr table
    entry = VADDR + code_off

    # ELF header
    e_ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)  # 64-bit, LSB
    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH",
        e_ident,
        2,      # ET_EXEC
        183,    # EM_AARCH64
        1,      # EV_CURRENT
        entry,
        ehdr_size,  # e_phoff
        0,      # e_shoff
        0,      # e_flags
        ehdr_size,
        phdr_size,
        1,      # e_phnum
        0, 0, 0,
    )

    filesz = code_off + len(code)
    phdr = struct.pack(
        "<IIQQQQQQ",
        1,          # PT_LOAD
        5,          # PF_R | PF_X
        0,          # p_offset
        VADDR,      # p_vaddr
        VADDR,      # p_paddr
        filesz,
        filesz,
        PAGE,       # p_align
    )

    with open(OUT, "wb") as f:
        f.write(ehdr)
        f.write(phdr)
        f.write(code)
    os.chmod(OUT, 0o755)
    print(f"wrote {OUT}: entry {entry:#x}, {filesz} bytes")


if __name__ == "__main__":
    sys.exit(main())
