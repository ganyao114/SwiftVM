"""Scalar immediate shifts, exact integer rounding and saturation expectations."""

import argparse
import struct

BASES = {"shl": 0x5F005400, "sli": 0x7F005400, "sri": 0x7F004400,
         "sshr": 0x5F000400, "ushr": 0x7F000400, "srshr": 0x5F002400,
         "urshr": 0x7F002400, "ssra": 0x5F001400, "usra": 0x7F001400,
         "srsra": 0x5F003400, "ursra": 0x7F003400,
         "sqshl": 0x5F007400, "uqshl": 0x7F007400, "sqshlu": 0x7F006400,
         "uqshrn": 0x7F009400, "uqrshrn": 0x7F009C00,
         "sqshrn": 0x5F009400, "sqrshrn": 0x5F009C00,
         "sqshrun": 0x7F008400, "sqrshrun": 0x7F008C00}
SATURATING_LEFT = ("sqshl", "uqshl", "sqshlu")
NARROW = ("uqshrn", "uqrshrn", "sqshrn", "sqrshrn", "sqshrun", "sqrshrun")
LEFT = ("shl", "sli") + SATURATING_LEFT
SIGNED = ("sshr", "srshr", "ssra", "srsra", "sqshrn", "sqrshrn", "sqshrun", "sqrshrun")
ROUND = ("srshr", "urshr", "srsra", "ursra", "uqrshrn", "sqrshrn", "sqrshrun")
ACCUMULATE = ("ssra", "usra", "srsra", "ursra")
MASK64 = (1 << 64) - 1


def corpus(case_type):
    cases = []
    initialize = tuple(word for reg in range(4) for word in
                       (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                        0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    capture = (0xD53B443B, 0xD53B441C) + tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))

    def add(op, bits, shift, value, alias, sticky, profile):
        source_bits = bits * 2 if op in NARROW else bits
        source_mask = (1 << source_bits) - 1
        source = ((value & source_mask) | ((~source_mask & MASK64) & 0xDEADBEEFDEADBEEF),
                  0xBADF00D5BADF00D5)
        vectors = [source if alias else (0xA55AA55AA55AA55A, MASK64), source,
                   (0x1020304050607080, 0x90A0B0C0D0E0F001), (0x0123456789ABCDEF, 0xFEDCBA9876543210)]
        seeds = {2 * reg + half + 1: vector[half] for reg, vector in enumerate(vectors) for half in range(2)}
        seeds.update({27: 0x9F | (sticky << 27), 28: 0x01400000})
        encoded = bits + shift if op in LEFT else 2 * bits - shift
        word = BASES[op] | (encoded << 16) | (0 if alias else 1 << 5)
        name = f"scalarshift_{op}_e{bits}_s{shift}_{profile}_{'alias' if alias else 'distinct'}_qc{sticky}"
        cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                               name, seeds, 0xB0000000, "scalarshift"))

    ordinary = [op for op in BASES if op not in SATURATING_LEFT + NARROW]
    for op in ordinary:
        shifts = range(64) if op in LEFT else range(1, 65)
        for shift in shifts:
            edges = (0, 1, MASK64, 1 << 63, (1 << 63) - 1, 0xAAAAAAAAAAAAAAAA)
            add(op, 64, shift, edges[shift % len(edges)], False, shift & 1, "full")
        for shift in ((0, 31, 63) if op in LEFT else (1, 32, 64)):
            for edge, value in enumerate((MASK64, 1 << 63, (1 << 63) - 1)):
                for alias in (False, True):
                    add(op, 64, shift, value, alias, edge & 1, f"boundary{edge}")
    for op in SATURATING_LEFT:
        for bits in (8, 16, 32, 64):
            for shift in (0, bits // 2, bits - 1):
                for edge, value in enumerate((0, -1, -(1 << (bits - 1)), (1 << (bits - 1)) - 1)):
                    for alias in (False, True):
                        for sticky in (0, 1):
                            add(op, bits, shift, value, alias, sticky, f"boundary{edge}")
    for op in NARROW:
        for bits in (8, 16, 32):
            for shift in (1, bits // 2, bits):
                # Each shift sees underflow, exact lower bound, upper overflow,
                # and an exact half-way remainder (including unsigned carry).
                limit = (1 << (bits - 1)) - 1 if op in ("sqshrn", "sqrshrn") else (1 << bits) - 1
                values = (-(1 << bits) << shift, -(1 << (bits - 1)) << shift,
                          (limit + 1) << shift, (limit << shift) + (1 << (shift - 1)))
                for edge, value in enumerate(values):
                    for alias in (False, True):
                        for sticky in (0, 1):
                            add(op, bits, shift, value, alias, sticky, f"boundary{edge}")
    if len(cases) != 2342:
        raise ValueError(f"Scalar shift coverage changed: {len(cases)}")
    return cases


def expected(case):
    _, op, width, count, _, alias, _ = case.name.split("_")
    bits, shift = int(width[1:]), int(count[1:])
    source_bits = 2 * bits if op in NARROW else bits
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    source = int.from_bytes(vectors[0 if alias == "alias" else 1][:source_bits // 8], "little")
    signed = source - (1 << source_bits) if source >> (source_bits - 1) else source
    old = int.from_bytes(vectors[0][:8], "little")
    mask, saturated = (1 << bits) - 1, False
    if op in LEFT:
        value = signed if op in ("sqshl", "sqshlu") else source
        result = value * (1 << shift)
        if op in SATURATING_LEFT:
            lower, upper = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if op == "sqshl" else (0, mask)
            clamped = max(lower, min(upper, result))
            saturated, result = clamped != result, clamped
        elif op == "sli":
            result = (result & mask) | (old & ((1 << shift) - 1))
    else:
        value = signed if op in SIGNED else source
        result = (value + ((1 << (shift - 1)) if op in ROUND else 0)) // (1 << shift)
        if op in NARROW:
            lower, upper = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if op in ("sqshrn", "sqrshrn") else (0, mask)
            clamped = max(lower, min(upper, result))
            saturated, result = clamped != result, clamped
        elif op in ACCUMULATE:
            result += old
        elif op == "sri":
            result |= old & (mask ^ (mask >> shift))
    vectors[0] = (result & mask).to_bytes(bits // 8, "little").ljust(16, b"\0")
    return {"memory": b"".join(vectors), "fpsr": case.seeds[27] | ((1 << 27) if saturated else 0),
            "fpcr": case.seeds[28]}


def verify():
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case)
    for case in cases:
        code = b"".join(word.to_bytes(4, "little") for word in case.instructions)
        if len(list(decoder.disasm(code, 0x400000))) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected code")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(0x400000, 4096)
        cpu.mem_map(differential.MEMORY_BASE & ~4095, 4096)
        cpu.mem_write(0x400000, code)
        for n in range(29):
            cpu.reg_write(getattr(registers, f"UC_ARM64_REG_X{n}"), case.seeds.get(n, 0x100 + n))
        cpu.reg_write(registers.UC_ARM64_REG_X29, differential.MEMORY_BASE)
        cpu.reg_write(registers.UC_ARM64_REG_NZCV, case.nzcv)
        cpu.emu_start(0x400000, 0x400000 + len(code))
        wanted = expected(case)
        if (bytes(cpu.mem_read(differential.MEMORY_BASE, 64)) != wanted["memory"] or
                cpu.reg_read(registers.UC_ARM64_REG_X27) != wanted["fpsr"] or
                cpu.reg_read(registers.UC_ARM64_REG_X28) != wanted["fpcr"] or
                cpu.reg_read(registers.UC_ARM64_REG_NZCV) != case.nzcv):
            raise ValueError(f"{case.name}: exact-integer rounding/clamp reference differs from Unicorn")
    print(f"PASS {len(cases)} scalar immediate shift integer/rounding/QC/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
