"""Bounded scalar SIMD integer corpus with Python integer/clamp expectations."""

import argparse
import random
import struct

BASES = {"abs": 0x5E20B800, "neg": 0x7E20B800, "sqabs": 0x5E207800,
         "sqneg": 0x7E207800, "cmgt": 0x5E208800, "cmge": 0x7E208800,
         "cmeq": 0x5E209800, "cmlt": 0x5E20A800, "cmle": 0x7E209800,
         "sqxtn": 0x5E214800, "uqxtn": 0x7E214800, "sqxtun": 0x7E212800,
         "addp": 0x5E31B800}
NARROW = ("sqxtn", "uqxtn", "sqxtun")
MASK64 = (1 << 64) - 1


def corpus(case_type):
    initialize = tuple(word for reg in range(4) for word in
                       (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                        0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    capture = (0xD53B443B, 0xD53B441C) + tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))
    cases, rng = [], random.Random(0x5CA1A2026)
    for op, base in BASES.items():
        widths = (8, 16, 32) if op in NARROW else (8, 16, 32, 64) if op in ("sqabs", "sqneg") else (64,)
        for bits in widths:
            source_bits = bits * 2 if op in NARROW else bits
            mask, sign, source_sign = (1 << bits) - 1, 1 << (bits - 1), 1 << (source_bits - 1)
            values = [0, 1, -1, -sign - 1, -sign, -sign + 1, sign - 1, sign, sign + 1,
                      mask, mask + 1, source_sign]
            inputs = [(f"edge{n}", value, sticky) for n, value in enumerate(values) for sticky in (0, 1)]
            inputs += [(f"random{n}", rng.getrandbits(source_bits), n & 1) for n in range(8)]
            for profile, value, sticky in inputs:
                for alias in (False, True):
                    source_mask = (1 << source_bits) - 1
                    pair_high = rng.getrandbits(64) if profile.startswith("random") else (
                        0, 1, MASK64, 1 << 63)[int(profile[4:]) % 4]
                    source = ((value & source_mask) | ((~source_mask & MASK64) & 0xDEADBEEFDEADBEEF),
                              pair_high if op == "addp" else 0xBADF00D5BADF00D5)
                    vectors = [source if alias else (0x0123456789ABCDEF, MASK64), source,
                               (0x1020304050607080, 0x90A0B0C0D0E0F001), (0xA55AA55AA55AA55A, 0x123456789ABCDEF0)]
                    seeds = {2 * reg + half + 1: vector[half] for reg, vector in enumerate(vectors) for half in range(2)}
                    seeds.update({27: 0x9F | (sticky << 27), 28: 0x01400000})
                    word = base | ((bits.bit_length() - 4) << 22) | (0 if alias else 1 << 5)
                    name = f"scalarmisc_{op}_e{bits}_{profile}_{'alias' if alias else 'distinct'}_qc{sticky}"
                    cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                                           name, seeds, 0xB0000000, "scalarmisc"))
    if len(cases) != 1600:
        raise ValueError(f"scalar miscellaneous coverage count changed: {len(cases)}")
    return cases


def expected(case):
    _, op, width, _, alias, _ = case.name.split("_")
    bits = int(width[1:])
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    source = vectors[0 if alias == "alias" else 1]
    source_bits = 2 * bits if op in NARROW else bits
    unsigned = int.from_bytes(source[:source_bits // 8], "little")
    signed = unsigned - (1 << source_bits) if unsigned & (1 << (source_bits - 1)) else unsigned
    mask, minimum, maximum = (1 << bits) - 1, -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    saturated = False
    if op in NARROW:
        value = unsigned if op == "uqxtn" else signed
        lower, upper = (minimum, maximum) if op == "sqxtn" else (0, mask)
        result = max(lower, min(upper, value))
        saturated = result != value
    elif op in ("abs", "neg", "sqabs", "sqneg"):
        result = abs(signed) if op.endswith("abs") else -signed
        if op.startswith("sq"):
            trimmed = max(minimum, min(maximum, result))
            saturated, result = trimmed != result, trimmed
    elif op == "addp":
        result = sum(struct.unpack("<QQ", source))
    else:
        condition = {"cmgt": signed > 0, "cmge": signed >= 0, "cmeq": signed == 0,
                     "cmlt": signed < 0, "cmle": signed <= 0}[op]
        result = mask if condition else 0
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
            raise ValueError(f"{case.name}: independent integer/QC reference differs from Unicorn")
    print(f"PASS {len(cases)} scalar miscellaneous integer/QC/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
