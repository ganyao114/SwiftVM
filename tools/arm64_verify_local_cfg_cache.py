#!/usr/bin/env python3
"""Real CCMP/CCMN local-IR-CFG cache corpus; complete declared 336-byte snapshots.

448 cases = W/X * 14 conditions * 4 seed profiles * CCMP/CCMN * register/imm5.
Each condition executes twice true and twice false, using two NZCV selections
per outcome. Flags liveness is pinned OFF: this corpus exercises the segmented
scalar cache, while the separate flags pass deliberately rejects local CFG.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import struct
import arm64_verify_cfg as base

d = base.d
CASE_COUNT = 448

def condition(code, flags):
    n, z, c, v = [bool(flags & (1 << bit)) for bit in (31, 30, 29, 28)]
    return (z, not z, c, not c, n, not n, v, not v,
            c and not z, not c or z, n == v, n != v,
            not z and n == v, z or n != v)[code]

def compare_flags(a, b, width, subtract):
    modulus = 1 << width
    sign = modulus >> 1
    a %= modulus
    b %= modulus
    signed_a = a - modulus if a >= sign else a
    signed_b = b - modulus if b >= sign else b
    total = a - b if subtract else a + b
    signed_total = signed_a - signed_b if subtract else signed_a + signed_b
    result = total % modulus
    carry = a >= b if subtract else total >= modulus
    overflow = not -sign <= signed_total < sign
    return ((result >= sign) << 31) | ((result == 0) << 30) | (carry << 29) | (overflow << 28)

def corpus():
    cases = []
    fixture = bytes((0x80 + 17 * i) & 255 for i in range(64))
    for width in (32, 64):
        sf = int(width == 64) << 31
        mask = (1 << width) - 1
        sign = 1 << (width - 1)
        pairs = ((0, 0), (mask, 1), (sign, mask), (sign - 1, 1))
        for code in range(14):
            for profile, (a, b) in enumerate(pairs):
                taken = bool(profile & 1)
                choices = [value << 28 for value in range(16) if condition(code, value << 28) == taken]
                initial_nzcv = choices[(profile // 2) % len(choices)]
                for subtract in (False, True):
                    for immediate in (False, True):
                        imm5 = (0, 1, 31, 17)[profile]
                        fallback = (code * 3 + profile * 5 + int(subtract) * 7 + int(immediate) * 11) & 15
                        rhs = imm5 if immediate else b
                        opcode = sf | (0x7a400000 if subtract else 0x3a400000)
                        opcode |= (0x800 if immediate else 0) | ((imm5 if immediate else 1) << 16) | (code << 12) | fallback
                        words = (0xD51B4439, 0xD51B441A,
                                 d.rr(sf | 0x0b000000, 8, 0, 1),
                                 d.rr(sf | 0x4a000000, 8, 8, 2),
                                 d.rr(sf | 0x0b000000, 9, 8, 1),
                                 opcode, 0xD53B420C,
                                 d.rr(sf | 0x1a000000, 10, 0, 1),
                                 d.rr(sf | 0x4a000000, 10, 10, 2),
                                 d.rr(sf | 0x0b000000, 11, 10, 3),
                                 d.rr(sf | 0x1a000000, 13, 11, 2),
                                 0xF90003A8, 0xF90007A9, 0xF9000BAA,
                                 0xF9000FAB, 0xF90013AC, 0xF90017AD,
                                 0xD53B443B, 0xD53B441C)
                        poison = 0xabcdef0100000000 if width == 32 else 0
                        seeds = {0: a | poison, 1: b | (0xfedcba9800000000 if width == 32 else 0),
                                 2: 0x123456789abcdef0, 3: 0xfedcba9876543210, 25: 0, 26: 0}
                        params = {'family': 'cache_straight', 'subfamily': 'local_cfg_conditional_compare',
                                  'requires_multiblock': False, 'width': width, 'condition': code,
                                  'profile': profile, 'a': a, 'b': b, 'subtract': subtract,
                                  'immediate': immediate, 'imm5': imm5, 'fallback_nzcv': fallback,
                                  'initial_nzcv': initial_nzcv, 'condition_taken': taken}
                        name = f'localcfg_{"ccmp" if subtract else "ccmn"}_{"imm" if immediate else "reg"}_w{width}_c{code:02d}_p{profile}'
                        case = d.Case(name, words, json.dumps(params, sort_keys=True), seeds,
                                      initial_nzcv, 'local_cfg_cache', fixture)
                        regs, memory, unused = base.initial(case)
                        regs[8] = (((regs[0] + regs[1]) & mask) ^ (regs[2] & mask)) & mask
                        regs[9] = (regs[8] + regs[1]) & mask
                        flags = compare_flags(a, rhs, width, subtract) if taken else fallback << 28
                        regs[12] = flags
                        carry = (flags >> 29) & 1
                        regs[10] = (((regs[0] + regs[1] + carry) & mask) ^ (regs[2] & mask)) & mask
                        regs[11] = (regs[10] + regs[3]) & mask
                        regs[13] = (regs[11] + regs[2] + carry) & mask
                        memory[:48] = struct.pack('<6Q', *(regs[i] for i in (8, 9, 10, 11, 12, 13)))
                        regs[27] = regs[28] = 0
                        raw = d.MAGIC + struct.pack('<Q31QQ', d.case_id(case), *regs, flags) + bytes(memory)
                        d.decode_snapshot(raw, case)
                        cases.append((case, raw, params))
    assert len(cases) == CASE_COUNT and len({case.name for case, _, _ in cases}) == CASE_COUNT
    return cases

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validate-only', action='store_true')
    parser.add_argument('--runner', type=Path)
    parser.add_argument('--runner-sha256')
    parser.add_argument('--runner-arg', action='append', default=[])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--flags-liveness', choices=('0',), default='0')
    parser.add_argument('--matrix', choices=('cache8',), default='cache8')
    parser.add_argument('--cache-stress', action='store_true', default=True)
    parser.add_argument('--require-coalescing', action='store_true', default=True)
    args = parser.parse_args()
    cases = corpus()
    if args.validate_only:
        for case, expected, params in cases:
            actual = base.execute_elf(case, d.make_elf(case))
            errors = base.compare(expected, actual, case)
            if errors:
                raise ValueError(f'{case.name}: {errors}')
        case, expected, _ = cases[0]
        corrupted = expected[:272] + bytes([expected[272] ^ 1]) + expected[273:]
        assert base.compare(expected, corrupted, case)
        print(f'PASS {CASE_COUNT} local-CFG complete math/Unicorn snapshots; SwiftVM executed=0')
        return
    if not args.runner or not args.output or not re.fullmatch('[0-9a-f]{64}', args.runner_sha256 or '') or args.timeout <= 0:
        parser.error('requires immutable runner/SHA, ext4 output and positive timeout')
    original_sources = base.sources
    base.sources = lambda: {**original_sources(), str(Path(__file__).resolve()): base.file_sha(Path(__file__))}
    original_checkpoint = base.checkpoint
    def checkpoint(path, report):
        report['scope'] = 'a64_local_cfg_cache_448_cache8'
        report['local_cfg_case_count'] = CASE_COUNT
        report['local_cfg_evidence_boundary'] = 'Real CCMP/CCMN local IR branches; snapshot covers X0-X30, NZCV and 64 memory bytes. No SP/Q/FPCR/FPSR full-state claim.'
        original_checkpoint(path, report)
    base.checkpoint = checkpoint
    base.run(args, cases)

if __name__ == '__main__':
    main()
