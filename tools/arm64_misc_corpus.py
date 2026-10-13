"""NEON bit/count/reversal/narrowing cases with an independent integer oracle.

Run ``python3 tools/arm64_misc_corpus.py --verify`` for reference-only validation.
The normal differential harness can consume corpus(), expected_memory(), and
expected_fpsr() without changing the reference-only validation path.
"""

import argparse
import random
import re
import struct


BASES = {"cnt": 0x0E205800, "rbit": 0x2E605800,
         "rev16": 0x0E201800, "rev32": 0x2E200800, "rev64": 0x0E200800,
         "clz": 0x2E204800, "cls": 0x0E204800,
         "xtn": 0x0E212800, "sqxtn": 0x0E214800,
         "uqxtn": 0x2E214800, "sqxtun": 0x2E212800}
NARROW = {"xtn", "sqxtn", "uqxtn", "sqxtun"}


def pack(values, bits):
    mask = (1 << bits) - 1
    return b"".join((value & mask).to_bytes(bits // 8, "little") for value in values)


def lanes(vector, bits, signed=False):
    result = [int.from_bytes(vector[n:n + bits // 8], "little")
              for n in range(0, len(vector), bits // 8)]
    return [v - (1 << bits) if signed and v >> (bits - 1) else v for v in result]


def profiles(op, bits, q):
    """Place each bit-pattern boundary in active lanes, including late lanes."""
    source_bits = bits * 2 if op in NARROW else bits
    active = 128 // source_bits if op in NARROW or q else 64 // source_bits
    total = 128 // source_bits
    mask, sign = (1 << source_bits) - 1, 1 << (source_bits - 1)
    data = []
    if op in ("cnt", "rbit"):
        values = list(range(256))
    elif op in ("clz", "cls"):
        values = [0, mask]
        for position in range(bits):
            values.extend((1 << position, mask ^ (1 << position)))
        values.extend((sign - 1, sign, sign + 1))
    elif op in NARROW:
        target_sign, target_mask = 1 << (bits - 1), (1 << bits) - 1
        values = [0, 1, -1, -target_sign - 1, -target_sign, -target_sign + 1,
                  target_sign - 2, target_sign - 1, target_sign, target_sign + 1,
                  target_mask - 1, target_mask, target_mask + 1,
                  -sign, -sign + 1, sign - 1, mask]
        # Uniform inputs prove QC stays clear for exact narrowing and becomes
        # set for each separate signed/unsigned boundary.
        data.extend(pack([value] * total, source_bits) for value in dict.fromkeys(values))
        if op != "xtn":
            saturated = -target_sign - 1 if op == "sqxtn" else target_mask + 1
            for index in range(total):
                isolated = [0] * total
                isolated[index] = saturated
                data.append(pack(isolated, source_bits))
    else:
        # Distinct element bytes make reversal direction and group size visible.
        values = [int.from_bytes(bytes((n * 29 + offset * 17) & 255
                                       for offset in range(source_bits // 8)), "little")
                  for n in range(total * 3)]
        values += [0, mask, sign, sign - 1]
    for begin in range(0, len(values), active):
        data.append(pack([values[(begin + index) % len(values)] for index in range(total)], source_bits))
    random_source = random.Random(0x6A09E667 + bits * 7 + q * 19 + list(BASES).index(op))
    data.extend(bytes(random_source.randrange(256) for _ in range(16)) for _ in range(4))
    return data


def corpus(case_type):
    result = []
    init = tuple(word for reg in range(4) for word in
                 (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                  0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    store = tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))
    widths = {"cnt": (8,), "rbit": (8,), "rev16": (8,), "rev32": (8, 16),
              "rev64": (8, 16, 32), "clz": (8, 16, 32), "cls": (8, 16, 32)}
    old = bytes.fromhex("aabbccddeeff00112233445566778899")
    untouched = [bytes.fromhex("0123456789abcdef1032547698badcfe"),
                 bytes.fromhex("f1e2d3c4b5a69788796a5b4c3d2e1f00")]
    for op, base in BASES.items():
        for bits in widths.get(op, (8, 16, 32)):
            for q in (0, 1):
                for profile, data in enumerate(profiles(op, bits, q)):
                    for alias in ("none", "src"):
                        for sticky in (0, 1):
                            vectors = [data if alias == "src" else old, data] + untouched
                            seeds = {reg * 2 + half + 1: int.from_bytes(vector[half * 8:half * 8 + 8], "little")
                                     for reg, vector in enumerate(vectors) for half in range(2)}
                            seeds[27] = 0x9F | (sticky << 27)
                            size = 0 if op == "rbit" else bits.bit_length() - 4
                            word = base | (q << 30) | (size << 22) | ((alias == "none") << 5)
                            name = f"misc_{op}_q{q}_e{bits}_p{profile}_{alias}_qc{sticky}"
                            result.append(case_type(name, (0xD51B443B,) + init + (word, 0xD53B443B) + store,
                                                    name, seeds, 0xB0000000, "misc"))
    return result


def expected(case):
    match = re.fullmatch(r"misc_([a-z0-9]+)_q([01])_e(8|16|32)_p\d+_(none|src)_qc[01]", case.name)
    if not match:
        raise ValueError(f"Unrecognized NEON miscellaneous case: {case.name}")
    op, q, bits, alias = match.groups()
    q, bits = int(q), int(bits)
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    source = vectors[0 if alias == "src" else 1]
    fpsr = case.seeds[27]
    if op in NARROW:
        values = lanes(source, bits * 2, op in ("sqxtn", "sqxtun"))
        minimum, maximum = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if op == "sqxtn" else (0, (1 << bits) - 1)
        if op != "xtn":
            narrowed = [max(minimum, min(value, maximum)) for value in values]
            if narrowed != values:
                fpsr |= 1 << 27
        else:
            narrowed = values
        output = pack(narrowed, bits)
        output = vectors[0][:8] + output if q else output + bytes(8)
    else:
        active = source[:16 if q else 8]
        if op == "cnt":
            output = bytes(value.bit_count() for value in active)
        elif op == "rbit":
            output = bytes(int(f"{value:08b}"[::-1], 2) for value in active)
        elif op.startswith("rev"):
            group = int(op[3:]) // bits
            values = lanes(active, bits)
            output = pack([value for start in range(0, len(values), group)
                           for value in reversed(values[start:start + group])], bits)
        else:
            values = lanes(active, bits)
            if op == "clz":
                counts = [bits - value.bit_length() for value in values]
            else:
                # Count the matching most-significant bits, excluding sign.
                counts = []
                for value in values:
                    binary = f"{value:0{bits}b}"
                    counts.append(len(binary) - len(binary.lstrip(binary[0])) - 1)
            output = pack(counts, bits)
        output = output.ljust(16, b"\0")
    vectors[0] = output
    return b"".join(vectors), fpsr


def expected_memory(case):
    return expected(case)[0]


def expected_fpsr(case):
    return expected(case)[1]


def verify():
    """Validate encodings and math directly in Unicorn without a VM runner."""
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as registers
    from arm64_differential import Case

    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(Case)
    for case in cases:
        code = struct.pack("<" + "I" * len(case.instructions), *case.instructions)
        decoded = list(decoder.disasm(code, 0x400000))
        op, q = case.name.split("_")[1:3]
        wanted = op + ("2" if op in NARROW and q == "q1" else "")
        if len(decoded) != len(case.instructions) or decoded[9].mnemonic != wanted:
            raise ValueError(f"{case.name}: invalid independent decoding")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(0x400000, 4096)
        cpu.mem_map(0x410000, 4096)
        cpu.mem_write(0x400000, code)
        for reg, value in case.seeds.items():
            cpu.reg_write(getattr(registers, f"UC_ARM64_REG_X{reg}"), value)
        cpu.reg_write(registers.UC_ARM64_REG_X29, 0x410000)
        cpu.reg_write(registers.UC_ARM64_REG_NZCV, case.nzcv)
        cpu.emu_start(0x400000, 0x400000 + len(code))
        memory, fpsr = expected(case)
        actual = bytes(cpu.mem_read(0x410000, 64))
        if actual != memory or cpu.reg_read(registers.UC_ARM64_REG_X27) != fpsr:
            raise ValueError(f"{case.name}: independent math/Unicorn mismatch, "
                             f"memory={actual.hex()} expected={memory.hex()}, "
                             f"fpsr={cpu.reg_read(registers.UC_ARM64_REG_X27):#x} expected={fpsr:#x}")
        if cpu.reg_read(registers.UC_ARM64_REG_NZCV) != case.nzcv:
            raise ValueError(f"{case.name}: NZCV changed")
    print(f"PASS {len(cases)} NEON miscellaneous independent math/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
