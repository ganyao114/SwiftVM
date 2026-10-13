"""Vector saturating immediate shifts with exact integer rounding and full snapshots."""

import argparse
import random
import struct

BASES = {"sqshl": 0x0F007400, "uqshl": 0x2F007400, "sqshlu": 0x2F006400,
         "sqshrn": 0x0F009400, "uqshrn": 0x2F009400, "sqshrun": 0x2F008400,
         "sqrshrn": 0x0F009C00, "uqrshrn": 0x2F009C00, "sqrshrun": 0x2F008C00}
LEFT = ("sqshl", "uqshl", "sqshlu")
SIGNED_RESULT = ("sqshl", "sqshrn", "sqrshrn")
UNSIGNED_SOURCE = ("uqshl", "uqshrn", "uqrshrn")
ROUND = ("sqrshrn", "uqrshrn", "sqrshrun")
MASK64 = (1 << 64) - 1


def corpus(case_type):
    cases = []
    rng = random.Random(0x173513AF926698FA)
    initialize = tuple(word for reg in range(4) for word in
                       (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                        0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    capture = (0xD53B443B, 0xD53B441C) + tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))
    for op, base in BASES.items():
        for bits in (8, 16, 32, 64):
            for q in (0, 1):
                narrow = op not in LEFT
                if (narrow and bits == 64) or (not narrow and bits == 64 and not q):
                    continue
                source_bits = bits * 2 if narrow else bits
                mask, sign = (1 << source_bits) - 1, 1 << (source_bits - 1)
                shifts = range(bits) if op in LEFT else range(1, bits + 1)
                for shift in shifts:
                    edges = (0, 1, mask, sign, sign - 1, sign + 1, (1 << bits) - 1, 1 << bits)
                    for alias in (0, 1):
                        for profile in (0, 1):
                            elements = [edges[(shift + lane) % len(edges)] if not profile else rng.getrandbits(source_bits)
                                        for lane in range(128 // source_bits)]
                            raw = b"".join((element & mask).to_bytes(source_bits // 8, "little") for element in elements)
                            source = struct.unpack("<QQ", raw)
                            vectors = [source if alias else (0xA55AA55AA55AA55A, MASK64), source,
                                       (0x1020304050607080, 0x90A0B0C0D0E0F001), (0x0123456789ABCDEF, 0xFEDCBA9876543210)]
                            seeds = {2 * reg + half + 1: vector[half] for reg, vector in enumerate(vectors) for half in range(2)}
                            seeds.update({27: 0x9F | (profile << 27), 28: 0x01400000})
                            encoded = bits + shift if op in LEFT else 2 * bits - shift
                            word = base | (q << 30) | (encoded << 16) | (0 if alias else 1 << 5)
                            name = f"vecsatshift_{op}_e{bits}_s{shift}_q{q}_a{alias}_p{profile}"
                            cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                                                   name, seeds, 0xB0000000, "vecsatshift"))
    if len(cases) != 4800:
        raise ValueError(f"Vector saturating shift coverage changed: {len(cases)}")
    return cases


def expected(case):
    _, op, width, count, arrangement, alias, _ = case.name.split("_")
    bits, shift, q = int(width[1:]), int(count[1:]), int(arrangement[1:])
    narrow = op not in LEFT
    source_bits = 2 * bits if narrow else bits
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    source = vectors[0 if int(alias[1:]) else 1]
    result, saturated = bytearray(), False
    lower, upper = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if op in SIGNED_RESULT else (0, (1 << bits) - 1)
    lanes = (64 if narrow else 128 if q else 64) // bits
    for lane in range(lanes):
        value = int.from_bytes(source[lane * source_bits // 8:(lane + 1) * source_bits // 8], "little")
        if op not in UNSIGNED_SOURCE and value >> (source_bits - 1):
            value -= 1 << source_bits
        if op in LEFT:
            value *= 1 << shift
        else:
            value = (value + ((1 << (shift - 1)) if op in ROUND else 0)) // (1 << shift)
        clamped = max(lower, min(upper, value))
        saturated |= clamped != value
        result.extend((clamped & ((1 << bits) - 1)).to_bytes(bits // 8, "little"))
    vectors[0] = vectors[0][:8] + bytes(result) if narrow and q else bytes(result).ljust(16, b"\0")
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
            raise ValueError(f"{case.name}: integer rounding/clamp reference differs from Unicorn")
    print(f"PASS {len(cases)} vector saturating shift cases: all 57 shapes and every immediate, alias/QC/Capstone/Unicorn")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
