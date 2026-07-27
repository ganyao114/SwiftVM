#!/usr/bin/env python3
"""Emit a one-instruction guest ELF that probes whether the x86 frontend
accepts a given VEX (AVX/AVX2) encoding.

DIAGNOSTIC TOOL, NOT A TEST FIXTURE.  It exists to answer one question without
touching CPUID: "if AVX were advertised and a guest executed this encoding,
would the decoder translate it or would the block trap as FALLBACK and kill the
process?"  Nothing here is wired into CMake or CTest.

The generated program is:

    mov  r13, <scratch page>        ; memory operands address [r13 + 0]
    <the encoding under test>
    mov  eax, 60 / mov edi, 42 / syscall

so the verdict is read straight off the exit status:

    42                             the decoder translated and ran it
    1  + "halted: reason 1"        ExitReason::IllegalCode -- unimplemented;
                                   a real guest process would die here
    1  + "halted: reason 2"        PageFatal -- a bad probe, not a decode gap

Usage:
    python3 gen_avx_probe_x86_64.py MAP PP OPCODE [options] -o out.elf
      MAP     0F | 0F38 | 0F3A
      PP      NP | 66 | F3 | F2
      OPCODE  hex, e.g. D7
      --l / --w                    set VEX.L (256-bit) / VEX.W
      --mem                        r/m is [r13+0] instead of a register
      --vvvv                       encode a real VEX.vvvv source (3-operand)
      --imm N                      trailing imm8 (hex or decimal)
      --reg N                      ModRM.reg override, for the /n group forms

    SVM_AVX=1 ./svm_translator_linux out.elf ; echo $?

Example -- VEX.128 vpmovmskb, the one AVX gap reachable from the in-tree glibc:
    python3 gen_avx_probe_x86_64.py 0F 66 D7 -o vpmovmskb128.elf
"""

import argparse
import os
import struct

VADDR = 0x400000
SCRATCH_OFF = 0x2000
SCRATCH = VADDR + SCRATCH_OFF
EHSIZE, PHSIZE = 64, 56
CODE_OFF = EHSIZE + PHSIZE

MAPS = {"0F": 1, "0F38": 2, "0F3A": 3}
PPS = {"NP": 0, "66": 1, "F3": 2, "F2": 3}

# (map, opcode) pairs that carry a trailing imm8.  Must match
# ImmediateForm() in runtime/frontend/x86/vex_decoder.cc: a disagreement
# mis-measures the instruction length and desynchronizes the decode stream.
IMM_MAP0F = {0xC2, 0x70, 0xC6, 0xC4, 0xC5, 0x71, 0x72, 0x73}


def encode(map_id, pp, opcode, l, w, mem, vvvv, imm, reg):
    """VEX.C4 3-byte form.  dst = reg 1, VEX.vvvv = reg 2, r/m = reg 3."""
    rex_b = 1 if mem else 0          # r13 as a base needs VEX.B; ymm3 does not
    vvvv_un = 2 if vvvv else 0       # 0 encodes the 1111b "no src1" field
    out = bytearray([0xC4])
    out.append((1 << 7) | (1 << 6) | ((0 if rex_b else 1) << 5) | (map_id & 0x1F))
    out.append(((1 if w else 0) << 7) | ((~vvvv_un & 0xF) << 3) |
               ((1 if l else 0) << 2) | (pp & 3))
    out.append(opcode)
    if map_id == 1 and opcode == 0x77:
        return bytes(out)            # vzeroupper / vzeroall have no ModRM
    regfield = reg if reg is not None else 1
    if mem:
        out.append(0x40 | ((regfield & 7) << 3) | 5)   # mod=01, rm=101 -> [r13+d8]
        out.append(0x00)
    else:
        out.append(0xC0 | ((regfield & 7) << 3) | 3)   # mod=11, rm=3
    if (map_id == 3) or (map_id == 1 and opcode in IMM_MAP0F):
        out.append((imm or 0) & 0xFF)
    return bytes(out)


def build(code, path):
    blob = bytearray(SCRATCH_OFF + 0x1000)
    blob[CODE_OFF:CODE_OFF + len(code)] = code
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    blob[0:EHSIZE] = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1,
                                 VADDR + CODE_OFF, EHSIZE, 0, 0,
                                 EHSIZE, PHSIZE, 1, 0, 0, 0)
    # One RWX PT_LOAD: code on page 0, scratch for memory operands on page 2.
    blob[EHSIZE:EHSIZE + PHSIZE] = struct.pack(
        "<IIQQQQQQ", 1, 7, 0, VADDR, VADDR, len(blob), len(blob), 0x1000)
    with open(path, "wb") as f:
        f.write(blob)
    os.chmod(path, 0o755)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map", choices=sorted(MAPS))
    ap.add_argument("pp", choices=sorted(PPS))
    ap.add_argument("opcode")
    ap.add_argument("--l", action="store_true", help="VEX.L (256-bit)")
    ap.add_argument("--w", action="store_true", help="VEX.W")
    ap.add_argument("--mem", action="store_true", help="r/m is memory")
    ap.add_argument("--vvvv", action="store_true", help="3-operand form")
    ap.add_argument("--imm", default=None)
    ap.add_argument("--reg", default=None, help="ModRM.reg for /n group forms")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    insn = encode(MAPS[a.map], PPS[a.pp], int(a.opcode, 16), a.l, a.w, a.mem,
                  a.vvvv, int(a.imm, 0) if a.imm else None,
                  int(a.reg, 0) if a.reg else None)
    code = (b"\x49\xbd" + struct.pack("<Q", SCRATCH)      # mov r13, scratch
            + insn
            + b"\xb8\x3c\x00\x00\x00"                     # mov eax, 60
            + b"\xbf\x2a\x00\x00\x00"                     # mov edi, 42
            + b"\x0f\x05")                                # syscall
    build(code, a.out)
    print("wrote %s: %s %s %s%s%s -> %s"
          % (a.out, a.map, a.pp, a.opcode,
             " L1" if a.l else "", " W1" if a.w else "",
             " ".join("%02x" % b for b in insn)))
    print("run: SVM_AVX=1 ./svm_translator_linux %s ; echo $?  "
          "(42 = handled, 1 = FALLBACK/fatal)" % a.out)


if __name__ == "__main__":
    main()
