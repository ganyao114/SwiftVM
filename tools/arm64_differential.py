#!/usr/bin/env python3
"""Build compiler-free AArch64 ELFs and compare Android with SwiftVM snapshots."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import random
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field


MAGIC = b"SVMA64\x00\x01"
CODE_BASE = 0x400000
DATA_BASE = 0x410000
MEMORY_BASE = DATA_BASE + 272
SNAPSHOT_SIZE = 336
# The legacy constants and default ELF remain v1. Its 336 bytes never claimed
# to contain PC/SP, vector registers, or FP control/status.
FULL_MAGIC = b"SVMA64\x00\x02"
FULL_SNAPSHOT_SIZE = 904
FULL_SNAPSHOT_BASE = DATA_BASE + 0x1000
VECTOR_SEEDS_BASE = DATA_BASE + 0x2000
CAPTURE_SP = DATA_BASE + 0xF000
CAPTURE_SCRATCH_OFFSET = 0x4000
FULL_DATA_SIZE = 0x20000
FULL_OFFSETS = {"pc": 24, "sp": 32, "registers": 40, "vectors": 288,
                "nzcv": 800, "fpcr": 808, "fpsr": 816, "memory_address": 824,
                "memory_size": 832, "memory": 840}
CAPTURE_BEFORE_HARNESS = 1
MASK64 = (1 << 64) - 1
ELF_ALIGNMENT = 0x10000  # Works with Android 4 KiB, 16 KiB and 64 KiB pages.
REMOTE = "/data/local/tmp/swiftvm-arm64-differential"


@dataclass(frozen=True)
class Case:
    name: str
    instructions: tuple[int, ...]
    assembly: str
    seeds: dict[int, int] = field(default_factory=dict)
    nzcv: int = 0x60000000
    family: str = "scalar"
    memory: bytes | None = None
    vectors: dict[int, int] = field(default_factory=dict)
    fpcr: int = 0
    fpsr: int = 0
    sp: int = CAPTURE_SP


def mov64(reg: int, value: int) -> list[int]:
    value &= MASK64
    return [0xD2800000 | ((value & 0xFFFF) << 5) | reg] + [
        0xF2800000 | (half << 21) | (((value >> (half * 16)) & 0xFFFF) << 5) | reg
        for half in range(1, 4)
        if (value >> (half * 16)) & 0xFFFF
    ]


def rr(base: int, dst: int, lhs: int, rhs: int, shift: int = 0) -> int:
    return base | (rhs << 16) | (shift << 10) | (lhs << 5) | dst


def corpus() -> list[Case]:
    cases = [
        Case("nop", (0xD503201F,), "nop"),
        Case("add_64", (rr(0x8B000000, 2, 0, 1),), "add x2, x0, x1"),
        Case("add_32_zero_extend", (rr(0x0B000000, 2, 0, 1),), "add w2, w0, w1",
             {0: 0xFFFFFFFF, 1: 2, 2: MASK64}),
        Case("adds_64_overflow", (rr(0xAB000000, 2, 0, 1),), "adds x2, x0, x1",
             {0: 0x7FFFFFFFFFFFFFFF, 1: 1}),
        Case("adds_64_carry", (rr(0xAB000000, 2, 0, 1),), "adds x2, x0, x1",
             {0: MASK64, 1: 1}),
        Case("subs_64_borrow", (rr(0xEB000000, 2, 0, 1),), "subs x2, x0, x1",
             {0: 0, 1: 1}),
        Case("subs_32_overflow", (rr(0x6B000000, 2, 0, 1),), "subs w2, w0, w1",
             {0: 0x80000000, 1: 1}),
        Case("and_shifted", (rr(0x8A000000, 2, 0, 1, 3),), "and x2, x0, x1, lsl #3",
             {0: 0x0123456789ABCDEF, 1: 0xFEDCBA9876543210}),
        Case("eor_32", (rr(0x4A000000, 2, 0, 1),), "eor w2, w0, w1",
             {0: 0x1234567812345678, 1: 0xFFEEDDCCBBAA9988}),
        Case("ands_clear_cv", (rr(0xEA000000, 2, 0, 1),), "ands x2, x0, x1",
             {0: 0x8000000000000000, 1: MASK64}, 0x30000000),
        Case("adc_carry", (rr(0x9A000000, 2, 0, 1),), "adc x2, x0, x1",
             {0: MASK64, 1: 0}, 0x20000000),
        Case("sbc_borrow", (rr(0xDA000000, 2, 0, 1),), "sbc x2, x0, x1",
             {0: 0, 1: 0}, 0),
        Case("load_store_64", (0xF90003A0, 0xF94003A2), "str x0, [x29]; ldr x2, [x29]",
             {0: 0xFEDCBA9876543210}),
        Case("load_signed_byte", (0x398003A2,), "ldrsb x2, [x29]"),
        Case("store_byte", (0x390007A0, 0x394007A2), "strb w0, [x29, #1]; ldrb w2, [x29, #1]",
             {0: 0xFACE}),
        Case("store_halfword", (0x790007A0, 0x794007A2), "strh w0, [x29, #2]; ldrh w2, [x29, #2]",
             {0: 0xDEADBEEF}),
        Case("store_word", (0xB90007A0, 0xB94007A2), "str w0, [x29, #4]; ldr w2, [x29, #4]",
             {0: 0x1122334455667788, 2: MASK64}),
        Case("branch_taken", (0x14000002, 0xD2800002), "b +8; mov x2, #0"),
        Case("cbz_taken", (0xB4000040, 0xD2800002), "cbz x0, +8; mov x2, #0",
             {0: 0}),
        Case("cbz_not_taken", (0xB4000040, 0xD2800002), "cbz x0, +8; mov x2, #0", {0: 1}),
        Case("beq_taken", (0x54000040, 0xD2800002), "b.eq +8; mov x2, #0", nzcv=0x40000000),
        Case("beq_not_taken", (0x54000040, 0xD2800002), "b.eq +8; mov x2, #0", nzcv=0),
        Case("csel_zero", (0x9A810002,), "csel x2, x0, x1, eq", nzcv=0x40000000),
        Case("csel_nonzero", (0x9A810002,), "csel x2, x0, x1, eq", nzcv=0),
    ]
    # Exercise every combination, including impossible arithmetic flag states.
    for flags in range(16):
        cases.append(Case(f"nzcv_{flags:x}", (0xD503201F,), "nop", nzcv=flags << 28))
    vector_init = (0x9E670020, 0x9EAF0040, 0x9E670061, 0x9EAF0081,
                   0x9E6700A2, 0x9EAF00C2)
    vector_store = (0x3D8003A0, 0x3D8007A1, 0x3D800BA2)
    vector_seeds = {1: 0x0706050403020100, 2: 0xFFFEFDFCFBFAF9F8,
                    3: 0x7766554433221100, 4: 0xFFEEDDCCBBAA9988,
                    5: 0x0123456789ABCDEF, 6: 0xFEDCBA9876543210}
    copy_operations = [
        ("dup_gpr_byte", 0x4E010C20, "dup v0.16b, w1"),
        ("dup_element_byte_high", 0x4E130401, "dup v1.16b, v0.b[9]"),
        ("dup_element_half_high", 0x4E160401, "dup v1.8h, v0.h[5]"),
        ("dup_element_double_high", 0x4E180401, "dup v1.2d, v0.d[1]"),
        ("ins_gpr_byte", 0x4E071C20, "ins v0.b[3], w1"),
        ("ins_element_alias_byte", 0x6E074C00, "ins v0.b[3], v0.b[9]"),
        ("ins_element_alias_word", 0x6E046400, "ins v0.s[0], v0.s[3]"),
        ("smov_signed_byte", 0x4E132C03, "smov x3, v0.b[9]"),
        ("smov_signed_halfword", 0x0E162C03, "smov w3, v0.h[5]"),
        ("umov_byte", 0x0E073C04, "umov w4, v0.b[3]"),
        ("umov_double_high", 0x4E183C03, "umov x3, v0.d[1]"),
    ]
    for name, word, assembly in copy_operations:
        cases.append(Case(name, vector_init + (word,) + vector_store, assembly,
                          dict(vector_seeds), 0xB0000000, "simd"))
    logic = [("and", 0x4E221C20), ("orr", 0x4EA21C20), ("orn", 0x4EE21C20),
             ("eor", 0x6E221C20), ("bic", 0x4E621C20), ("bsl", 0x6E621C20),
             ("bit", 0x6EA21C20), ("bif", 0x6EE21C20)]
    for name, word in logic:
        for width in (64, 128):
            opcode = word if width == 128 else word & ~(1 << 30)
            lanes = "16b" if width == 128 else "8b"
            cases.append(Case(f"neon_{name}_{width}", vector_init + (opcode,) + vector_store,
                              f"{name} v0.{lanes}, v1.{lanes}, v2.{lanes}",
                              dict(vector_seeds), 0xB0000000, "simd"))
    cases += [
        Case("fmov_single_bits", (0x1E270020, 0x1E260002, 0x3D8003A0),
             "fmov s0, w1; fmov w2, s0; str q0, [x29]", {1: 0xFFC12345, 2: MASK64}, family="simd"),
        Case("fmov_double_bits", (0x9E670020, 0x9E660002, 0x3D8003A0),
             "fmov d0, x1; fmov x2, d0; str q0, [x29]", {1: 0xFFF8123456789ABC}, family="simd"),
        Case("fmov_high64_bits", (0x9E670020, 0x9EAF0040, 0x9EAE0003, 0x3D8003A0),
             "fmov d0, x1; fmov v0.d[1], x2; fmov x3, v0.d[1]; str q0, [x29]",
             {1: 0x0123456789ABCDEF, 2: 0xFEDCBA9876543210}, family="simd"),
        Case("fneg_single_signed_zero", (0x1E270020, 0x1E214000, 0x1E260002, 0x3D8003A0),
             "fmov s0, w1; fneg s0, s0; fmov w2, s0; str q0, [x29]", {1: 0x80000000}, family="simd"),
        Case("fabs_single_nan_bits", (0x1E270020, 0x1E20C000, 0x1E260002, 0x3D8003A0),
             "fmov s0, w1; fabs s0, s0; fmov w2, s0; str q0, [x29]", {1: 0xFFC12345}, family="simd"),
        Case("fneg_double_bits", (0x9E670020, 0x1E614000, 0x9E660002, 0x3D8003A0),
             "fmov d0, x1; fneg d0, d0; fmov x2, d0; str q0, [x29]", {1: 0x3FF0000000000000}, family="simd"),
    ]
    def vector_case(name, opcode, assembly, seeds=None, flags=0xB0000000):
        cases.append(Case(name, vector_init + (opcode,) + vector_store, assembly,
                          dict(vector_seeds if seeds is None else seeds), flags, "simd"))

    for operation, base in (("add", 0x4E228420), ("sub", 0x6E228420), ("mul", 0x4E229C20)):
        for width in (64, 128):
            for size in range(4 if operation != "mul" and width == 128 else 3):
                bits = 8 << size
                word = base | (size << 22)
                if width == 64:
                    word &= ~(1 << 30)
                suffix = {8: "b", 16: "h", 32: "s", 64: "d"}[bits]
                shape = f"{width // bits}{suffix}"
                vector_case(f"neon_{operation}_{width}_e{bits}", word,
                            f"{operation} v0.{shape}, v1.{shape}, v2.{shape}")
    rng = random.Random(0x53564D)
    for operation, base, width, size in (("add", 0x4E228420, 128, 0),
                                        ("sub", 0x6E228420, 64, 1),
                                        ("mul", 0x4E229C20, 128, 2)):
        seeds = {reg: rng.getrandbits(64) for reg in range(1, 7)}
        word = (base | (size << 22)) & (~(1 << 30) if width == 64 else MASK64)
        vector_case(f"neon_{operation}_{width}_e{8 << size}_random", word,
                    f"{operation} with deterministic random vector seeds", seeds)
    for width in (64, 128):
        word = 0x6E205820 & (~(1 << 30) if width == 64 else MASK64)
        vector_case(f"neon_not_{width}", word, f"not v0.{width // 8}b, v1.{width // 8}b")
    vector_case("neon_not_128_random", 0x6E205820, "not with deterministic random vector seeds",
                {reg: rng.getrandbits(64) for reg in range(1, 7)})
    for target_bits in (8, 16, 32):
        for shift in (1, target_bits // 2, target_bits):
            for high in (False, True):
                word = 0x0F008420 | ((target_bits * 2 - shift) << 16) | (int(high) << 30)
                op = "shrn2" if high else "shrn"
                vector_case(f"neon_{op}_e{target_bits}_s{shift}", word,
                            f"{op} target={target_bits} source={target_bits * 2} shift={shift}")
    for width in (64, 128):
        for index in (0, width // 16, width // 8 - 1):
            word = 0x2E020020 | (index << 11) | ((width == 128) << 30)
            vector_case(f"neon_ext_{width}_i{index}", word,
                        f"ext v0.{width // 8}b, v1.{width // 8}b, v2.{width // 8}b, #{index}")

    def modified_immediate(imm8, cmode, op):
        return 0x4F000400 | (op << 29) | (cmode << 12) | ((imm8 >> 5) << 16) | ((imm8 & 31) << 5)

    for shift, cmode in ((8, 12), (16, 13)):
        for inverse in (False, True):
            op = "mvni" if inverse else "movi"
            vector_case(f"neon_{op}_msl{shift}", modified_immediate(0x12, cmode, int(inverse)),
                        f"{op} v0.4s, #0x12, msl #{shift}")
    for imm in (0, 0xA5, 0xFF):
        vector_case(f"neon_movi_bytemask_{imm:02x}", modified_immediate(imm, 14, 1),
                    f"movi v0.2d with imm8 byte mask {imm:#x}")
    vector_case("neon_fmov_imm32", modified_immediate(0xF0, 15, 0), "fmov v0.4s, #-1.0")
    vector_case("neon_fmov_imm64", modified_immediate(0x70, 15, 1), "fmov v0.2d, #1.0")
    for bits in (32, 64):
        word = 0x1E201000 | (0x70 << 13) | ((bits == 64) << 22)
        vector_case(f"fmov_immediate_{bits}", word, f"fmov {'d' if bits == 64 else 's'}0, #1.0")
    for condition in range(16):
        bits = 32 if condition % 2 == 0 else 64
        flags = ((condition * 7 + 5) % 16) << 28
        word = 0x1E220C20 | (condition << 12) | ((bits == 64) << 22)
        vector_case(f"fcsel_{bits}_c{condition:x}", word,
                    f"fcsel scalar{bits} condition={condition:#x}", flags=flags)
    fixture = bytes((0x80 + i * 17) & 0xFF for i in range(64))
    for bits in (8, 16, 32, 64):
        size = {8: 0, 16: 1, 32: 2, 64: 3}[bits]
        old = int.from_bytes(fixture[:bits // 8], "little")
        for success in (False, True):
            word = 0x08A07FA1 | (size << 30)
            seeds = {0: old if success else old ^ 1, 1: 0x0011223344556677}
            cases.append(Case(f"cas_{bits}_{'match' if success else 'mismatch'}", (word,),
                              f"CAS{bits} x0 expected / x1 desired / [x29]", seeds,
                              0xB0000000, "atomic"))
    for bits in (32, 64):
        size = {32: 2, 64: 3}[bits]
        old = int.from_bytes(fixture[:bits // 8], "little")
        for success in (False, True):
            cases.append(Case(f"casal_{bits}_{'match' if success else 'mismatch'}",
                              (0x08E0FFA1 | (size << 30),), "CAS acquire-release",
                              {0: old if success else old ^ 1, 1: 0x0011223344556677},
                              0xB0000000, "atomic"))
    for operation, opcode in (("ldadd", 0), ("ldclr", 1), ("ldeor", 2), ("ldset", 3), ("swp", 8)):
        for bits in (8, 16, 32, 64):
            size = {8: 0, 16: 1, 32: 2, 64: 3}[bits]
            cases.append(Case(f"lse_{operation}_{bits}",
                              (0x382003A1 | (size << 30) | (opcode << 12),),
                              f"{operation}{bits} x0 operand / x1 old value / [x29]",
                              {0: 0x0123456789ABCDEF, 1: MASK64}, 0xB0000000, "atomic"))
    cases.append(Case("lse_ldaddal_64", (0xF8E003A1,), "LDADD acquire-release",
                      {0: 0x0123456789ABCDEF, 1: MASK64}, 0xB0000000, "atomic"))
    for order, word in (("a", 0xF8A003A1), ("l", 0xF86003A1)):
        cases.append(Case(f"lse_ldadd{order}_64", (word,), "LDADD acquire/release",
                          {0: 0x0123456789ABCDEF, 1: MASK64}, 0xB0000000, "atomic"))
    for operation, opcode in (("ldclr", 1), ("ldeor", 2), ("ldset", 3), ("swp", 8)):
        cases.append(Case(f"lse_{operation}al_32", (0xB8E003A1 | (opcode << 12),), "LSE acquire-release",
                          {0: 0x0123456789ABCDEF, 1: MASK64}, 0xB0000000, "atomic"))
    for bits in (8, 16, 32, 64):
        size = {8: 0, 16: 1, 32: 2, 64: 3}[bits]
        cases.append(Case(f"ldar_{bits}", (0x08DFFFA1 | (size << 30),), "LDAR acquire",
                          {1: MASK64}, 0xB0000000, "atomic"))
        cases.append(Case(f"stlr_{bits}", (0x089FFFA0 | (size << 30),), "STLR release",
                          {0: 0x0123456789ABCDEF}, 0xB0000000, "atomic"))
    for operation, opcode in (("ldsmax", 4), ("ldsmin", 5), ("ldumax", 6), ("ldumin", 7)):
        for bits in (8, 16, 32, 64):
            size = {8: 0, 16: 1, 32: 2, 64: 3}[bits]
            for suffix, ordering in (("", 0), ("a", 1), ("l", 2), ("al", 3)):
                word = 0x382003A1 | (size << 30) | (opcode << 12)
                word |= ((ordering & 1) << 23) | ((ordering >> 1) << 22)
                operand = (1 << (bits - 1)) if ordering & 1 else (1 << (bits - 1)) - 1
                if ordering & 2:
                    operand ^= (1 << bits) - 1
                memory = None
                # Unicorn 2.1.4 incorrectly treats sub-64 signed LSE extrema
                # as unsigned. Use nonnegative lanes in this oracle corpus;
                # C++ mathematical tests cover negative/cross-sign lanes.
                if operation.startswith("lds") and bits < 64:
                    count = bits // 8
                    memory = (0x10).to_bytes(count, "little") + fixture[count:]
                    operand = (0x08, 0x20, 0x10, (1 << (bits - 1)) - 1)[ordering]
                cases.append(Case(f"lse_{operation}{suffix}_{bits}", (word,),
                                  f"{operation}{suffix} access width {bits}", {0: operand, 1: MASK64},
                                  0xB0000000, "atomic", memory))
    old_pair = int.from_bytes(fixture[:8], "little")
    for suffix, ordering in (("", 0), ("a", 1), ("l", 2), ("al", 3)):
        for match in (False, True):
            opcode = 0x08207FA2 | ((ordering & 1) << 22) | ((ordering >> 1) << 15)
            seeds = {0: (old_pair & 0xFFFFFFFF) | 0x1111111100000000,
                     1: (old_pair >> 32) ^ (0 if match else 1),
                     2: 0x2222222201234567, 3: 0x3333333389ABCDEF}
            cases.append(Case(f"casp{suffix}_32_{'match' if match else 'mismatch'}", (opcode,),
                              f"CASP{suffix} W0,W1,W2,W3,[X29]", seeds, 0xB0000000, "atomic"))
    from arm64_simd_corpus import corpus as simd_corpus
    cases.extend(simd_corpus(Case))
    from arm64_fp_corpus import corpus as fp_corpus
    cases.extend(fp_corpus(Case))
    from arm64_fp_fixed_corpus import corpus as fixed_corpus
    cases.extend(fixed_corpus(Case))
    from arm64_shift_corpus import corpus as shift_corpus
    cases.extend(shift_corpus(Case))
    from arm64_sat_corpus import corpus as sat_corpus
    cases.extend(sat_corpus(Case))
    from arm64_half_corpus import corpus as half_corpus
    cases.extend(half_corpus(Case))
    from arm64_struct_corpus import corpus as struct_corpus
    cases.extend(struct_corpus(Case))
    from arm64_single_mem_corpus import corpus as single_corpus
    cases.extend(single_corpus(Case))
    from arm64_indexed_corpus import corpus as indexed_corpus
    cases.extend(indexed_corpus(Case))
    from arm64_scalar_simd_corpus import corpus as scalar_simd_corpus
    cases.extend(scalar_simd_corpus(Case))
    from arm64_misc_corpus import corpus as misc_corpus
    cases.extend(misc_corpus(Case))
    from arm64_flagm_corpus import corpus as flagm_corpus
    cases.extend(flagm_corpus(Case))
    from arm64_crypto_corpus import corpus as crypto_corpus
    cases.extend(crypto_corpus(Case))
    from arm64_crc_corpus import corpus as crc_corpus
    cases.extend(crc_corpus(Case))
    from arm64_sha_corpus import corpus as sha_corpus
    cases.extend(sha_corpus(Case))
    from arm64_scalar_misc_corpus import corpus as scalar_misc_corpus
    cases.extend(scalar_misc_corpus(Case))
    from arm64_rcpc_corpus import corpus as rcpc_corpus
    cases.extend(rcpc_corpus(Case))
    from arm64_scalar_shift_corpus import corpus as scalar_shift_corpus
    cases.extend(scalar_shift_corpus(Case))
    from arm64_casp_corpus import corpus as casp_corpus
    cases.extend(casp_corpus(Case))
    from arm64_scalar_multiply_corpus import corpus as scalar_multiply_corpus
    cases.extend(scalar_multiply_corpus(Case))
    from arm64_scalar_mulhigh_corpus import corpus as scalar_mulhigh_corpus
    cases.extend(scalar_mulhigh_corpus(Case))
    from arm64_dot_corpus import corpus as dot_corpus
    cases.extend(dot_corpus(Case))
    from arm64_saturating_multiply_corpus import corpus as saturating_multiply_corpus
    cases.extend(saturating_multiply_corpus(Case))
    from arm64_scalar_pairwise_fp_corpus import corpus as scalar_pairwise_fp_corpus
    cases.extend(scalar_pairwise_fp_corpus(Case))
    from arm64_fp16_unary_corpus import corpus as fp16_unary_corpus
    cases.extend(fp16_unary_corpus(Case))
    from arm64_fp16_binary_corpus import corpus as fp16_binary_corpus
    cases.extend(fp16_binary_corpus(Case))
    from arm64_vector_fp_binary_corpus import corpus as vector_fp_binary_corpus
    cases.extend(vector_fp_binary_corpus(Case))
    from arm64_fma_nan_probe import corpus as fma_nan_corpus
    cases.extend(fma_nan_corpus(Case))
    from arm64_scalar_fp16_misc_corpus import corpus as scalar_fp16_misc_corpus
    cases.extend(scalar_fp16_misc_corpus(Case))
    from arm64_vector_fma_corpus import corpus as vector_fma_corpus
    cases.extend(vector_fma_corpus(Case))
    from arm64_fmulx_corpus import corpus as fmulx_corpus
    cases.extend(fmulx_corpus(Case))
    from arm64_reciprocal_step_corpus import corpus as reciprocal_step_corpus
    cases.extend(reciprocal_step_corpus(Case))
    from arm64_vector_sat_shift_corpus import corpus as vector_sat_shift_corpus
    cases.extend(vector_sat_shift_corpus(Case))
    from arm64_indexed_fp_corpus import corpus as indexed_fp_corpus
    cases.extend(indexed_fp_corpus(Case))
    from arm64_vector_doubling_long_corpus import corpus as vector_doubling_long_corpus
    cases.extend(vector_doubling_long_corpus(Case))
    from arm64_vector_fp_unary_corpus import corpus as vector_fp_unary_corpus
    cases.extend(vector_fp_unary_corpus(Case))
    from arm64_fp_compare_corpus import corpus as fp_compare_corpus
    cases.extend(fp_compare_corpus(Case))
    from arm64_fp_integer_corpus import corpus as fp_integer_corpus
    cases.extend(fp_integer_corpus(Case))
    from arm64_fp_precision_corpus import corpus as fp_precision_corpus
    cases.extend(fp_precision_corpus(Case))
    from arm64_fp_estimate_corpus import corpus as fp_estimate_corpus
    cases.extend(fp_estimate_corpus(Case))
    from arm64_fp_reduction_corpus import corpus as fp_reduction_corpus
    cases.extend(fp_reduction_corpus(Case))
    from arm64_unsigned_estimate_corpus import corpus as unsigned_estimate_corpus
    cases.extend(unsigned_estimate_corpus(Case))
    from arm64_fp_fixed_simd_corpus import corpus as fp_fixed_simd_corpus
    cases.extend(fp_fixed_simd_corpus(Case))
    from arm64_frint_range_corpus import corpus as frint_range_corpus
    cases.extend(frint_range_corpus(Case))
    from arm64_polynomial_corpus import corpus as polynomial_corpus
    cases.extend(polynomial_corpus(Case))
    from arm64_scalar_variable_shift_corpus import corpus as scalar_variable_shift_corpus
    cases.extend(scalar_variable_shift_corpus(Case))
    from arm64_mixed_sat_corpus import corpus as mixedsat_corpus
    cases.extend(mixedsat_corpus(Case))
    from arm64_pairwise_widen_corpus import corpus as pairwiden_corpus
    cases.extend(pairwiden_corpus(Case))
    from arm64_high_narrow_corpus import corpus as highnarrow_corpus
    cases.extend(highnarrow_corpus(Case))
    from arm64_absolute_long_corpus import corpus as abslong_corpus
    cases.extend(abslong_corpus(Case))
    from arm64_absolute_narrow_corpus import corpus as absnarrow_corpus
    cases.extend(absnarrow_corpus(Case))
    from arm64_halving_corpus import corpus as halving_corpus
    cases.extend(halving_corpus(Case))
    names = [case.name for case in cases]
    if len(names) != len(set(names)):
        raise ValueError("duplicate case names would overwrite differential artifacts")
    return cases


def case_id(case: Case) -> int:
    return int.from_bytes(hashlib.sha256(case.name.encode()).digest()[:8], "little")


def snapshot_protocol(version: int) -> dict:
    if version not in (1, 2):
        raise ValueError(f"unknown ARM64 snapshot version: {version}")
    return {"version": version, "size": SNAPSHOT_SIZE if version == 1 else FULL_SNAPSHOT_SIZE,
            "sampling_point": "after case instructions, before capture harness",
            "fields": ["X0-X30", "NZCV", "memory[64]"] if version == 1 else
                      ["PC", "SP", "X0-X30", "Q0-Q31", "NZCV", "FPCR", "FPSR", "memory[64]"],
            "memory_address": MEMORY_BASE, "memory_size": 64}


def vector_seed(case: Case, reg: int) -> int:
    # Every untouched vector, including the high registers, has distinct bits.
    return case.vectors.get(reg, (0x3210000000000000 + reg) |
                            ((0x4320000000000100 + reg) << 64))


def capture_scratch_addresses(sp: int, mapped_size: int = FULL_DATA_SIZE) -> tuple[int, int]:
    """Audit both scratch stores against the actual SP and ELF data mapping."""
    if not 0 <= sp <= MASK64:
        raise ValueError("v2 capture requires an unsigned 64-bit SP")
    first, second = sp + CAPTURE_SCRATCH_OFFSET, sp + CAPTURE_SCRATCH_OFFSET + 8
    if first < DATA_BASE or second + 8 > DATA_BASE + mapped_size:
        raise ValueError("capture scratch is outside the mapped fixture region")
    for address, size, name in ((MEMORY_BASE, 64, "observed memory"),
                                (FULL_SNAPSHOT_BASE, FULL_SNAPSHOT_SIZE, "snapshot"),
                                (VECTOR_SEEDS_BASE, 512, "vector seeds")):
        if first < address + size and second + 8 > address:
            raise ValueError(f"capture scratch aliases {name}")
    return first, second


def full_harness(case: Case) -> tuple[list[int], int]:
    capture_scratch_addresses(case.sp)
    words = mov64(0, VECTOR_SEEDS_BASE)
    for reg in range(32):
        words.append(0x3DC00000 | (reg << 10) | reg)  # ldr qN, [x0, #N*16]
    words += mov64(0, case.sp)
    words.append(0x9100001F)  # mov sp, x0
    words += mov64(0, case.fpcr) + [0xD51B4400]  # msr fpcr, x0
    words += mov64(0, case.fpsr) + [0xD51B4420]  # msr fpsr, x0
    for reg in range(31):
        default = MEMORY_BASE if reg == 29 else DATA_BASE if reg == 30 else 0x100 + reg
        words += mov64(reg, case.seeds.get(reg, default))
    words += mov64(28, case.nzcv) + [0xD51B421C]
    words += mov64(28, case.seeds.get(28, 0x11C))
    words += case.instructions
    capture_index = len(words)
    # Save temporaries far from guest memory, without changing the actual SP.
    # Guest cases may have moved SP into or beside the observed memory region.
    words.append(0xF90003E0 | ((CAPTURE_SCRATCH_OFFSET // 8) << 10))
    words.append(0xF90003E1 | (((CAPTURE_SCRATCH_OFFSET + 8) // 8) << 10))
    words += mov64(0, FULL_SNAPSHOT_BASE)
    def store(reg, offset):
        words.append(0xF9000000 | ((offset // 8) << 10) | reg)  # str xN, [x0, #offset]
    for reg in range(2, 31):
        store(reg, FULL_OFFSETS["registers"] + reg * 8)
    words.append(0xF94003E1 | ((CAPTURE_SCRATCH_OFFSET // 8) << 10))
    store(1, FULL_OFFSETS["registers"])
    words.append(0xF94003E1 | (((CAPTURE_SCRATCH_OFFSET + 8) // 8) << 10))
    store(1, FULL_OFFSETS["registers"] + 8)
    delta = (capture_index - len(words)) * 4
    words.append(0x10000001 | ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5))  # adr x1, capture
    store(1, FULL_OFFSETS["pc"])
    words.append(0x910003E1)  # mov x1, sp (unchanged actual SP)
    store(1, FULL_OFFSETS["sp"])
    for reg in range(32):
        words.append(0x3D800000 | (((FULL_OFFSETS["vectors"] + reg * 16) // 16) << 10) | reg)
    for opcode, field_name in ((0xD53B4201, "nzcv"), (0xD53B4401, "fpcr"), (0xD53B4421, "fpsr")):
        words.append(opcode)
        store(1, FULL_OFFSETS[field_name])
    words += mov64(1, MEMORY_BASE)
    for offset in range(0, 64, 8):
        words.append(0xF9400000 | ((offset // 8) << 10) | (1 << 5) | 2)
        store(2, FULL_OFFSETS["memory"] + offset)
    words += mov64(0, 1) + mov64(1, FULL_SNAPSHOT_BASE) + mov64(2, FULL_SNAPSHOT_SIZE) + mov64(8, 64)
    words += [0xD4000001, 0xF1000000 | (FULL_SNAPSHOT_SIZE << 10) | 31, 0x9A9F07E0]
    words += mov64(8, 93) + [0xD4000001]
    return words, CODE_BASE + 0x1000 + capture_index * 4


def snapshot_boundary(case: Case, version: int = 1) -> int | None:
    return full_harness(case)[1] if version == 2 else None


def full_state_cases() -> list[Case]:
    """Focused v2 probes, kept out of the historical registered corpus."""
    return [
        Case("state_preserve_all", (0xD503201F,), "nop; preserve every architectural field",
             {0: MASK64, 1: 0x123456789abcdef0, 29: 0xabcdef, 30: 0x76543210},
             family="state", vectors={31: (1 << 127) | 0x12345678}, fpcr=0x00400000, fpsr=0x08000091),
        Case("state_guest_x30_sp", (0x910083FF, rr(0x8B000000, 30, 0, 1)),
             "add sp, sp, #32; add x30, x0, x1", {0: 0x1234, 1: 0x5678}, family="state"),
        Case("state_guest_q31", (0x4EA01C1F,), "orr v31.16b, v0.16b, v0.16b", family="state"),
        Case("state_scalar_fp_clears_upper", (0x1E60401F,), "fmov d31, d0", family="state"),
        Case("state_guest_fpcr_fpsr", (0xD51B4400, 0xD51B4421),
             "msr fpcr, x0; msr fpsr, x1", {0: 0x00C00000, 1: 0x0800009F}, family="state"),
    ]


def encode_full_snapshot(case: Case, *, pc: int, sp: int, registers: list[int],
                         vectors: list[int], nzcv: int, fpcr: int, fpsr: int,
                         memory: bytes) -> bytes:
    if len(registers) != 31 or len(vectors) != 32 or len(memory) != 64:
        raise ValueError("v2 snapshot requires 31 X, 32 Q, and 64 memory bytes")
    return (FULL_MAGIC + struct.pack("<QIIQQ31Q", case_id(case), FULL_SNAPSHOT_SIZE,
            CAPTURE_BEFORE_HARNESS, pc, sp, *registers) +
            b"".join(value.to_bytes(16, "little") for value in vectors) +
            struct.pack("<5Q", nzcv, fpcr, fpsr, MEMORY_BASE, len(memory)) + memory)


def make_elf(case: Case, snapshot_version: int = 1) -> bytes:
    snapshot_protocol(snapshot_version)
    words: list[int] = []
    for reg in range(29):
        words += mov64(reg, case.seeds.get(reg, 0x100 + reg))
    words += mov64(29, MEMORY_BASE)
    words += mov64(30, DATA_BASE)
    words += mov64(28, case.nzcv)
    words.append(0xD51B421C)  # msr nzcv, x28
    words += mov64(28, case.seeds.get(28, 0x11C))
    words += case.instructions
    for reg in range(31):
        offset = 16 + reg * 8
        words.append(0xF9000000 | ((offset // 8) << 10) | (30 << 5) | reg)
    words.append(0xD53B4200)  # mrs x0, nzcv (after saving original x0)
    words.append(0xF9000000 | ((264 // 8) << 10) | (30 << 5))
    words += mov64(0, 1) + mov64(1, DATA_BASE) + mov64(2, SNAPSHOT_SIZE) + mov64(8, 64)
    words.append(0xD4000001)  # svc #0 / write
    # Linux write is not allowed to silently truncate the snapshot.
    words.append(0xF1000000 | (SNAPSHOT_SIZE << 10) | 31)  # cmp x0, #336
    words.append(0x9A9F07E0)  # cset x0, ne
    words += mov64(8, 93)
    words.append(0xD4000001)  # svc #0 / exit
    if snapshot_version == 2:
        words, _ = full_harness(case)
    code = struct.pack(f"<{len(words)}I", *words)
    if len(code) + 0x1000 > DATA_BASE - CODE_BASE:
        raise ValueError("case code exceeds executable segment")
    memory = case.memory if case.memory is not None else bytes((0x80 + i * 17) & 0xFF for i in range(64))
    if len(memory) != 64:
        raise ValueError("case fixture must contain exactly 64 bytes")
    data = MAGIC + struct.pack("<Q", case_id(case)) + bytes(256) + memory
    if snapshot_version == 2:
        data = bytearray(data.ljust(VECTOR_SEEDS_BASE - DATA_BASE + 512, b"\0"))
        snapshot = encode_full_snapshot(case, pc=0, sp=0, registers=[0] * 31, vectors=[0] * 32,
                                        nzcv=0, fpcr=0, fpsr=0, memory=bytes(64))
        start = FULL_SNAPSHOT_BASE - DATA_BASE
        data[start:start + len(snapshot)] = snapshot
        start = VECTOR_SEEDS_BASE - DATA_BASE
        data[start:start + 512] = b"".join(vector_seed(case, reg).to_bytes(16, "little") for reg in range(32))
    ident = b"\x7fELF\x02\x01\x01\x00" + bytes(8)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 183, 1, CODE_BASE + 0x1000,
                         64, 0, 0, 64, 56, 2, 0, 0, 0)
    executable_size = 0x1000 + len(code)
    data_offset = (executable_size + ELF_ALIGNMENT - 1) & ~(ELF_ALIGNMENT - 1)
    program_headers = struct.pack("<IIQQQQQQ", 1, 5, 0, CODE_BASE, CODE_BASE,
                                  executable_size, executable_size, ELF_ALIGNMENT)
    program_headers += struct.pack("<IIQQQQQQ", 1, 6, data_offset, DATA_BASE, DATA_BASE,
                                   len(data), len(data) if snapshot_version == 1 else FULL_DATA_SIZE, ELF_ALIGNMENT)
    return (header + program_headers).ljust(0x1000, b"\0") + code + bytes(
        data_offset - executable_size) + data


def decode_snapshot(raw: bytes, case: Case, snapshot_version: int | None = None) -> dict:
    version = 2 if raw[:8] == FULL_MAGIC else 1
    if snapshot_version is not None and version != snapshot_version:
        raise ValueError(f"{case.name}: snapshot protocol version mismatch")
    if version == 2:
        if len(raw) != FULL_SNAPSHOT_SIZE:
            raise ValueError(f"{case.name}: expected {FULL_SNAPSHOT_SIZE} v2 stdout bytes, got {len(raw)}")
        identity, size, point = struct.unpack_from("<QII", raw, 8)
        if identity != case_id(case) or size != FULL_SNAPSHOT_SIZE or point != CAPTURE_BEFORE_HARNESS:
            raise ValueError(f"{case.name}: v2 snapshot identity/size/sampling point mismatch")
        memory_address, memory_size = struct.unpack_from("<2Q", raw, FULL_OFFSETS["memory_address"])
        if memory_address != MEMORY_BASE or memory_size != 64:
            raise ValueError(f"{case.name}: v2 memory descriptor mismatch")
        regs = struct.unpack_from("<31Q", raw, FULL_OFFSETS["registers"])
        nzcv, fpcr, fpsr = struct.unpack_from("<3Q", raw, FULL_OFFSETS["nzcv"])
        if nzcv & ~0xF0000000:
            raise ValueError(f"{case.name}: NZCV has reserved bits: {nzcv:#x}")
        return {"case": case.name, "snapshot_version": 2, "sampling_point": point,
                "pc": f"0x{struct.unpack_from('<Q', raw, FULL_OFFSETS['pc'])[0]:016x}",
                "sp": f"0x{struct.unpack_from('<Q', raw, FULL_OFFSETS['sp'])[0]:016x}",
                "registers": {f"x{i}": f"0x{v:016x}" for i, v in enumerate(regs)},
                "vectors": {f"q{i}": f"0x{int.from_bytes(raw[288 + i * 16:304 + i * 16], 'little'):032x}"
                            for i in range(32)},
                "nzcv": f"0x{nzcv:08x}", "fpcr": f"0x{fpcr:08x}", "fpsr": f"0x{fpsr:08x}",
                "memory_address": f"0x{memory_address:016x}", "memory": raw[840:].hex()}
    if len(raw) != SNAPSHOT_SIZE:
        raise ValueError(f"{case.name}: expected {SNAPSHOT_SIZE} stdout bytes, got {len(raw)}")
    if raw[:8] != MAGIC or struct.unpack_from("<Q", raw, 8)[0] != case_id(case):
        raise ValueError(f"{case.name}: snapshot magic/version/case id mismatch")
    regs = struct.unpack_from("<31Q", raw, 16)
    nzcv = struct.unpack_from("<Q", raw, 264)[0]
    if nzcv & ~0xF0000000:
        raise ValueError(f"{case.name}: NZCV has reserved bits: {nzcv:#x}")
    return {"case": case.name, "registers": {f"x{i}": f"0x{v:016x}" for i, v in enumerate(regs)},
            "nzcv": f"0x{nzcv:08x}", "memory": raw[272:].hex()}


def differences(expected: dict, actual: dict) -> list[str]:
    if expected.get("snapshot_version", 1) != actual.get("snapshot_version", 1):
        return ["snapshot protocol versions differ; incomplete and full state cannot be compared"]
    diff = [f"{reg}: native={value}, SwiftVM={actual['registers'][reg]}"
            for reg, value in expected["registers"].items() if value != actual["registers"][reg]]
    if expected.get("snapshot_version") == 2:
        for field_name in ("pc", "sp"):
            if expected[field_name] != actual[field_name]:
                diff.append(f"{field_name.upper()}: native={expected[field_name]}, SwiftVM={actual[field_name]}")
        diff += [f"{reg}: native={value}, SwiftVM={actual['vectors'][reg]}"
                 for reg, value in expected["vectors"].items() if value != actual["vectors"][reg]]
        for field_name in ("fpcr", "fpsr", "memory_address"):
            if expected[field_name] != actual[field_name]:
                diff.append(f"{field_name.upper()}: native={expected[field_name]}, SwiftVM={actual[field_name]}")
    if expected["nzcv"] != actual["nzcv"]:
        diff.append(f"NZCV: native={expected['nzcv']}, SwiftVM={actual['nzcv']}")
    a, b = bytes.fromhex(expected["memory"]), bytes.fromhex(actual["memory"])
    if len(a) != len(b):
        diff.append(f"memory size: native={len(a)}, SwiftVM={len(b)}")
    diff += [f"memory[{i:#x}]: native={lhs:#04x}, SwiftVM={rhs:#04x}"
             for i, (lhs, rhs) in enumerate(zip(a, b)) if lhs != rhs]
    return diff


def snapshot_negative_controls(raw: bytes, case: Case) -> dict:
    """Deliberately corrupt real captured bytes and require field detection.

    These are comparison controls, not additional executed guest cases.
    The caller must supply its actual v2 stdout, rather than a fabricated
    architectural state or a snapshot constructed from the oracle.
    """
    state = decode_snapshot(raw, case, 2)
    controls = (("PC", 24, 1), ("SP", 32, 1), ("x30", 40 + 30 * 8, 1),
                ("q0", 288, 1), ("q31", 288 + 31 * 16, 1), ("q31", 288 + 31 * 16 + 15, 1),
                ("FPCR", 808, 1), ("FPSR", 816, 1), ("NZCV", 803, 0x10),
                ("memory[0x3f]", 840 + 63, 1))
    fields = []
    for field_name, offset, mask in controls:
        broken = bytearray(raw)
        broken[offset] ^= mask
        errors = differences(state, decode_snapshot(bytes(broken), case, 2))
        if len(errors) != 1 or not errors[0].startswith(field_name + ":"):
            raise ValueError(f"{case.name}: {field_name} corruption escaped the full-state comparison: {errors}")
        fields.append({"field": field_name, "byte_offset": offset, "rejected": True})
    return {"source": "actual SwiftVM stdout", "source_sha256": hashlib.sha256(raw).hexdigest(),
            "executed": len(fields), "rejected": len(fields), "controls": fields}


def run_command(command: list[str], timeout: float) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, timeout=timeout)
    if result.returncode:
        stderr = result.stderr.decode("utf-8", "replace")[-6000:]
        stdout = result.stdout.decode("utf-8", "replace")[-1000:]
        raise RuntimeError(f"command failed ({result.returncode}): {command!r}\n{stderr}\n{stdout}")
    return result


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def build(output: Path, cases: list[Case], snapshot_version: int = 1) -> None:
    existing = output / "corpus.json"
    if existing.is_file():
        prior = json.loads(existing.read_text(encoding="utf-8"))
        if prior.get("snapshot_protocol", {"version": 1})["version"] != snapshot_version:
            raise ValueError("output contains a different snapshot protocol; select a new directory")
    output.mkdir(parents=True, exist_ok=True)
    entries = []
    for case in cases:
        elf = output / f"{case.name}.elf"
        generated = make_elf(case, snapshot_version)
        # Reuse only byte-identical ELF inputs. References and manifests are
        # regenerated; a stale or corrupt ELF is replaced before it is hashed.
        if not elf.is_file() or elf.read_bytes() != generated:
            elf.write_bytes(generated)
        elf.chmod(0o755)
        entry = {"name": case.name, "sha256": sha(elf), "assembly": case.assembly,
                 "initial_nzcv": hex(case.nzcv), "seeds": {f"x{k}": hex(v) for k, v in case.seeds.items()}}
        if snapshot_version == 2:
            entry.update(initial_sp=hex(case.sp), initial_fpcr=hex(case.fpcr), initial_fpsr=hex(case.fpsr),
                         vector_seeds={f"q{i}": f"0x{vector_seed(case, i):032x}" for i in range(32)},
                         capture_pc=hex(snapshot_boundary(case, 2)))
        entries.append(entry)
    write_json(output / "corpus.json", {"version": 1, "snapshot_protocol": snapshot_protocol(snapshot_version), "cases": entries})


def adb_executable(explicit: str | None) -> str:
    if explicit:
        return explicit
    found = shutil.which("adb") or shutil.which("adb.exe")
    if found:
        return found
    candidates = [Path(os.environ.get("ANDROID_HOME", "")) / "platform-tools" / "adb.exe",
                  Path(os.environ.get("LOCALAPPDATA", "")) / "Android/Sdk/platform-tools/adb.exe"]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("adb not found; pass --adb /path/to/adb")


def native(args: argparse.Namespace, cases: list[Case], output: Path) -> None:
    snapshot_version = getattr(args, "snapshot_version", 1)
    adb = adb_executable(args.adb)
    listing = run_command([adb, "devices"], args.timeout).stdout.decode("utf-8", "replace")
    devices = [line.split()[0] for line in listing.splitlines()[1:] if line.strip().endswith("\tdevice")]
    serial = args.serial
    if serial and serial not in devices:
        raise RuntimeError(f"ADB device {serial!r} is not connected/authorized; available: {devices}")
    if not serial:
        if len(devices) != 1:
            raise RuntimeError(f"expected one authorized adb device, found {len(devices)}; pass --serial: {devices}")
        serial = devices[0]
    prefix = [adb, "-s", serial]
    abi = run_command(prefix + ["shell", "getprop", "ro.product.cpu.abilist"], args.timeout).stdout.decode().strip()
    if "arm64-v8a" not in abi:
        raise RuntimeError(f"device has no arm64-v8a ABI: {abi}")
    native_dir = output / "native"
    native_dir.mkdir(exist_ok=True)
    manifest = {"version": 1, "snapshot_protocol": snapshot_protocol(snapshot_version),
                "complete": False, "serial": serial, "abi": abi, "cases": {}}
    write_json(native_dir / "manifest.json", manifest)
    run_command(prefix + ["shell", "mkdir", "-p", REMOTE], args.timeout)
    for case in cases:
        elf = output / f"{case.name}.elf"
        remote = f"{REMOTE}/{case.name}.elf"
        run_command(prefix + ["push", str(elf), remote], args.timeout)
        run_command(prefix + ["shell", "chmod", "755", remote], args.timeout)
        result = run_command(prefix + ["exec-out", remote], args.timeout)
        state = decode_snapshot(result.stdout, case, snapshot_version)
        (native_dir / f"{case.name}.bin").write_bytes(result.stdout)
        write_json(native_dir / f"{case.name}.json", state)
        manifest["cases"][case.name] = {"elf_sha256": sha(elf), "snapshot_sha256": hashlib.sha256(result.stdout).hexdigest()}
        write_json(native_dir / "manifest.json", manifest)
    manifest["complete"] = True
    write_json(native_dir / "manifest.json", manifest)
    print(f"captured {len(cases)} ARM64 native cases on {serial}")


def wsl_path(path: Path) -> str:
    value = str(path.resolve())
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", value)
    if not match:
        raise ValueError(f"WSL conversion requires a drive path: {value}")
    return f"/mnt/{match[1].lower()}/" + match[2].replace("\\", "/")


def compare(args: argparse.Namespace, cases: list[Case], output: Path) -> None:
    snapshot_version = getattr(args, "snapshot_version", 1)
    if not args.runner:
        raise RuntimeError("--runner must point to svm_translator_linux (use --wsl for a Linux runner on Windows)")
    if not args.wsl and not shutil.which(args.runner):
        raise RuntimeError(f"runner is absent or not executable: {args.runner}; zero SwiftVM cases executed")
    native_dir = output / "native"
    manifest = json.loads((native_dir / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("complete") is not True or manifest.get("version") != 1:
        raise RuntimeError("native capture is incomplete or uses an unknown protocol")
    if manifest.get("snapshot_protocol", {"version": 1})["version"] != snapshot_version:
        raise RuntimeError("native capture belongs to a different snapshot protocol")
    translated = output / "translated"
    translated.mkdir(exist_ok=True)
    report = {"version": 1, "snapshot_protocol": snapshot_protocol(snapshot_version),
              "complete": False, "executed": 0, "passed": 0, "failed": 0, "cases": []}
    write_json(output / "report.json", report)
    for case in cases:
        elf = output / f"{case.name}.elf"
        entry = manifest["cases"].get(case.name)
        if not entry or entry["elf_sha256"] != sha(elf):
            raise RuntimeError(f"{case.name}: native reference is absent or belongs to a different ELF")
        raw = (native_dir / f"{case.name}.bin").read_bytes()
        if hashlib.sha256(raw).hexdigest() != entry["snapshot_sha256"]:
            raise RuntimeError(f"{case.name}: native snapshot digest mismatch")
        expected = decode_snapshot(raw, case, snapshot_version)
        command = [args.runner, *args.runner_arg, str(elf)]
        if args.wsl:
            command = ["wsl"] + (["-d", args.wsl] if args.wsl != "default" else []) + [
                "--exec", args.runner, *args.runner_arg, wsl_path(elf)]
        try:
            result = subprocess.run(command, capture_output=True, timeout=args.timeout)
            (translated / f"{case.name}.bin").write_bytes(result.stdout)
            (translated / f"{case.name}.stderr").write_bytes(result.stderr)
            if result.returncode:
                raise RuntimeError(f"runner exited with {result.returncode}: "
                                   + result.stderr.decode("utf-8", "replace")[-6000:])
            actual = decode_snapshot(result.stdout, case, snapshot_version)
            write_json(translated / f"{case.name}.json", actual)
            diff = differences(expected, actual)
        except subprocess.TimeoutExpired as error:
            (translated / f"{case.name}.bin").write_bytes(error.stdout or b"")
            (translated / f"{case.name}.stderr").write_bytes(error.stderr or b"")
            diff = [str(error)]
        except (OSError, RuntimeError, ValueError) as error:
            diff = [str(error)]
        report["executed"] += 1
        report["failed" if diff else "passed"] += 1
        report["cases"].append({"name": case.name, "differences": diff})
        write_json(output / "report.json", report)
        print(f"{'FAIL' if diff else 'PASS'} {case.name}")
        for line in diff[:8]:
            print(f"  {line}")
    report["complete"] = True
    write_json(output / "report.json", report)
    print(f"executed={report['executed']} passed={report['passed']} failed={report['failed']}")
    if report["failed"]:
        raise RuntimeError(f"{report['failed']} differential case(s) failed; see {output / 'report.json'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["build", "native", "compare", "run"])
    parser.add_argument("--output", type=Path, default=Path("build/arm64-differential"))
    parser.add_argument("--case", action="append", default=[], help="exact case name; repeat to select multiple")
    parser.add_argument("--snapshot-version", type=int, choices=[1, 2], default=1,
                        help="1: legacy 336-byte GPR/NZCV/memory; 2: 904-byte complete architectural state")
    parser.add_argument("--group", choices=["all", "scalar", "simd", "atomic", "fp", "fp_fixed", "shift", "sat", "half", "struct", "single", "indexed", "scalarneon", "misc", "flagm", "crypto", "crc", "sha", "scalarmisc", "rcpc", "scalarshift", "casp", "scalarmul", "scalarmulhigh", "dot", "satmul", "pairfp", "fp16unary", "fp16binary", "vecfp", "fmanan", "misc16", "vecfma", "fmulx", "recipstep", "vecsatshift", "indexedfp", "veclong", "vecunary", "fpcompare", "fpinteger", "fpprecision", "fpestimate", "fpreduction", "unsignedestimate", "fpfixed", "frintrange", "polynomial", "scalarvarshift", "mixedsat", "pairwiden", "highnarrow", "abslong", "absnarrow", "halving"], default="all")
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    parser.add_argument("--runner", help="svm_translator_linux executable (Linux absolute path with --wsl)")
    parser.add_argument("--runner-arg", action="append", default=[])
    parser.add_argument("--wsl", nargs="?", const="default", help="invoke runner through WSL, optionally naming a distro")
    parser.add_argument("--timeout", type=float, default=20)
    args = parser.parse_args()
    try:
        cases = corpus()
        if args.group != "all":
            cases = [case for case in cases if case.family == args.group]
        if args.case:
            known = {case.name for case in cases}
            unknown = set(args.case) - known
            if unknown:
                raise ValueError(f"unknown case(s): {sorted(unknown)}")
            cases = [case for case in cases if case.name in args.case]
        if not cases:
            raise ValueError("zero selected cases")
        output = args.output.resolve()
        build(output, cases, args.snapshot_version)
        if args.mode in ("native", "run"):
            native(args, cases, output)
        if args.mode in ("compare", "run"):
            compare(args, cases, output)
        if args.mode == "build":
            print(f"built {len(cases)} static AArch64 ELFs in {output}; no execution performed")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
