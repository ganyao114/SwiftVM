#!/usr/bin/env python3
"""Emit minimal guest ELFs carrying deliberately malformed / hostile x86-64
instruction streams.

TEST FIXTURE GENERATOR for run_malformed_guest_tests.sh.

The property under test is *isolation*, not correctness: whatever the guest
byte stream is, the HOST translator process must survive it.  A guest that
dies (ExitReason::IllegalCode / PageFatal) is a pass; a host that takes an
unhandled SIGSEGV/SIGBUS, or aborts on an internal invariant, is a failure.

Two shapes are produced:

  ``flat``  guest image at 0x400000 spanning 0x3000; a small prologue
            (``movabs r13, <scratch>``), the payload under test, then
            exit(42).  Reaching exit means the payload translated and ran.

  ``edge``  guest image spanning exactly 0x4000 -- one macOS/arm64 host page --
            with the entry point placed so the payload ends flush with the end
            of the mapping.  This is the case that catches an instruction-fetch
            window that reads past the end of guest memory: the decoder hands
            distorm a fixed 16-byte window, so a decode starting in the last
            bytes of the last mapped page runs off the end of the host mapping
            too and faults in host code, where the JIT's guest-fault recovery
            cannot reach it.

Usage:
    gen_malformed_guest_x86_64.py flat <hexbytes> -o out.elf
    gen_malformed_guest_x86_64.py edge <hexbytes> -o out.elf
"""

import argparse
import struct

VADDR = 0x400000
SCRATCH_OFF = 0x2000
EHSIZE, PHSIZE = 64, 56
CODE_OFF = EHSIZE + PHSIZE
# One macOS arm64 host page. An image spanning exactly this much ends flush
# with the host mapping, so any over-read past it is a host fault.
EDGE_SPAN = 0x4000


def _headers(entry, filesz, memsz):
    eh = bytearray(EHSIZE)
    eh[0:4] = b"\x7fELF"
    eh[4], eh[5], eh[6] = 2, 1, 1  # ELF64, little endian, version 1
    struct.pack_into("<HHI", eh, 16, 2, 0x3E, 1)  # ET_EXEC, EM_X86_64
    struct.pack_into("<QQQ", eh, 24, entry, EHSIZE, 0)
    struct.pack_into("<IHHHHHH", eh, 48, 0, EHSIZE, PHSIZE, 1, 0, 0, 0)
    ph = bytearray(PHSIZE)  # PT_LOAD, RWX, offset 0
    struct.pack_into("<IIQQQQQQ", ph, 0, 1, 7, 0, VADDR, VADDR, filesz, memsz, 0x1000)
    return bytes(eh), bytes(ph)


def build_flat(payload, out):
    code = bytearray()
    code += b"\x49\xbd" + struct.pack("<Q", VADDR + SCRATCH_OFF)  # movabs r13, scratch
    code += payload
    code += b"\xb8\x3c\x00\x00\x00"  # mov eax, 60  (__NR_exit)
    code += b"\xbf\x2a\x00\x00\x00"  # mov edi, 42
    code += b"\x0f\x05"              # syscall
    eh, ph = _headers(VADDR + CODE_OFF, CODE_OFF + len(code), SCRATCH_OFF + 0x1000)
    with open(out, "wb") as f:
        f.write(eh + ph + bytes(code))


def build_edge(payload, out):
    if len(payload) > EDGE_SPAN - CODE_OFF:
        raise SystemExit("payload too long for an edge image")
    entry_off = EDGE_SPAN - len(payload)
    image = bytearray(EDGE_SPAN)
    image[entry_off:entry_off + len(payload)] = payload
    eh, ph = _headers(VADDR + entry_off, EDGE_SPAN, EDGE_SPAN)
    image[0:EHSIZE] = eh
    image[EHSIZE:EHSIZE + PHSIZE] = ph
    with open(out, "wb") as f:
        f.write(bytes(image))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("shape", choices=("flat", "edge"))
    ap.add_argument("hexbytes", help="payload, e.g. 'c4e1699bcb' or 'c4 e1 69 9b cb'")
    ap.add_argument("-o", required=True)
    a = ap.parse_args()
    data = bytes.fromhex(a.hexbytes.replace(" ", "").replace(",", ""))
    (build_flat if a.shape == "flat" else build_edge)(data, a.o)
