#!/usr/bin/env python3
"""Wrap x87_bench_x86_64.S in a minimal static x86_64 Linux ELF."""

import os
import struct
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "x87_bench_x86_64.S")
OBJ = os.path.join(HERE, "x87_bench_x86_64.o")
RAW = os.path.join(HERE, "x87_bench_x86_64.bin")
OUT = os.path.join(HERE, "x87_bench_x86_64")
CLANG = os.environ.get("CLANG", "clang")
OBJCOPY = os.environ.get("LLVM_OBJCOPY", "/opt/homebrew/opt/llvm/bin/llvm-objcopy")

subprocess.check_call([CLANG, "--target=x86_64-linux-gnu", "-c", SRC, "-o", OBJ])
subprocess.check_call([OBJCOPY, "-O", "binary", "--only-section=.text", OBJ, RAW])
with open(RAW, "rb") as source:
    code = source.read()

vaddr = 0x400000
ehsize, phsize = 64, 56
code_offset = ehsize + phsize
ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
header = struct.pack(
    "<16sHHIQQQIHHHHHH",
    ident, 2, 62, 1, vaddr + code_offset, ehsize, 0, 0,
    ehsize, phsize, 1, 0, 0, 0,
)
size = code_offset + len(code)
program = struct.pack(
    "<IIQQQQQQ", 1, 5, 0, vaddr, vaddr, size, size, 0x1000
)
with open(OUT, "wb") as output:
    output.write(header)
    output.write(program)
    output.write(code)
os.chmod(OUT, 0o755)
print(f"wrote {OUT}: {size} bytes")
