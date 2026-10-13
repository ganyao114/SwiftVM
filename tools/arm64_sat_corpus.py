"""Saturating integer SIMD cases; unbounded Python arithmetic is the oracle."""

import re
import struct
from arm64_shift_corpus import lanes, pack


def corpus(case_type):
    result = []
    init = tuple(word for reg in range(4) for word in
                 (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                  0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    store = tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))
    bases = {"sqadd": 0x0E200C00, "uqadd": 0x2E200C00,
             "sqsub": 0x0E202C00, "uqsub": 0x2E202C00}
    for op, base in bases.items():
        for width in (64, 128):
            for size, bits in enumerate((8, 16, 32, 64)):
                if width == 64 and bits == 64:
                    continue
                maximum, sign = (1 << bits) - 1, 1 << (bits - 1)
                for pattern in ("positive", "negative", "exact", "inactive"):
                    if pattern == "positive":
                        a, b = (sign - 1, 1) if op[0] == "s" else (maximum, 1)
                        if op.endswith("sub"):
                            b = maximum if op[0] == "s" else maximum
                            a = sign - 1 if op[0] == "s" else 0
                    elif pattern == "negative":
                        a, b = (sign, maximum) if op.endswith("add") else (sign, 1)
                        if op[0] == "u":
                            a, b = (maximum - 1, 2) if op.endswith("add") else (0, 1)
                    else:
                        a, b = (12, 3)
                    av = [a for _ in range(128 // bits)]
                    bv = [b for _ in av]
                    if pattern == "inactive":
                        # A Q=0 operation must ignore saturation in the discarded upper lanes.
                        for index in range(64 // bits, len(av)):
                            av[index] = sign - 1 if op[0] == "s" else maximum
                            bv[index] = 1 if op.endswith("add") else maximum
                    for alias in ("none", "src", "rhs"):
                        for sticky in (0, 1):
                            old = bytes.fromhex("aabbccddeeff00112233445566778899")
                            vectors = [pack(av, bits) if alias == "src" else old,
                                       pack(av, bits), pack(bv, bits), old]
                            seeds = {reg * 2 + half + 1: int.from_bytes(vector[half * 8:half * 8 + 8], "little")
                                     for reg, vector in enumerate(vectors) for half in range(2)}
                            seeds[27] = 0x9F | (sticky << 27)
                            rn, rd = (0 if alias == "src" else 1), (2 if alias == "rhs" else 0)
                            word = base | ((width == 128) << 30) | (size << 22) | (2 << 16) | (rn << 5) | rd
                            name = f"sat_{op}_{width}_e{bits}_{pattern}_{alias}_qc{sticky}"
                            result.append(case_type(name, (0xD51B443B,) + init + (word, 0xD53B443B) + store,
                                                    name, seeds, 0xB0000000, "sat"))
    return result


def expected(case):
    match = re.fullmatch(r"sat_(sqadd|uqadd|sqsub|uqsub)_(64|128)_e(8|16|32|64)_[a-z]+_(none|src|rhs)_qc[01]", case.name)
    if not match:
        raise ValueError(f"Unrecognized saturation case: {case.name}")
    op, width, bits, alias = match.groups()
    width, bits = int(width), int(bits)
    signed = op[0] == "s"
    minimum, maximum = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    lhs = lanes(vectors[0 if alias == "src" else 1][:width // 8], bits, signed)
    rhs = lanes(vectors[2][:width // 8], bits, signed)
    raw = [a + b if op.endswith("add") else a - b for a, b in zip(lhs, rhs)]
    clipped = [max(minimum, min(value, maximum)) for value in raw]
    vectors[2 if alias == "rhs" else 0] = pack(clipped, bits).ljust(16, b"\0")
    fpsr = case.seeds[27] | ((raw != clipped) << 27)
    return b"".join(vectors), fpsr
