"""Vector shifts with boundary operands and an independent integer lane oracle."""

import re
import struct


def pack(values, bits):
    return b"".join((value & ((1 << bits) - 1)).to_bytes(bits // 8, "little") for value in values)


def lanes(vector, bits, signed=False):
    values = [int.from_bytes(vector[n:n + bits // 8], "little") for n in range(0, len(vector), bits // 8)]
    return [v - (1 << bits) if signed and v & (1 << (bits - 1)) else v for v in values]


def corpus(case_type):
    result = []
    init = tuple(word for reg in range(4) for word in
                 (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                  0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    store = tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))

    def add(op, width, bits, shift, alias, word, variable=False, sticky=True):
        mask = (1 << bits) - 1
        boundary = [mask, 1 << (bits - 1), (1 << (bits - 1)) - 1, 1,
                    0, mask - 1, (1 << (bits - 2)) + 1, 3]
        data = pack([boundary[n % len(boundary)] for n in range(128 // bits)], bits)
        old = bytes.fromhex("aabbccddeeff00112233445566778899")
        counts = pack([((0xA55AA55AA55AA500 | (shift & 255)) & mask)
                       for _ in range(128 // bits)], bits)
        vectors = [data if alias == "src" else old, data, counts if variable else old, old]
        seeds = {reg * 2 + half + 1: int.from_bytes(vector[half * 8:half * 8 + 8], "little")
                 for reg, vector in enumerate(vectors) for half in range(2)}
        seeds[27] = 0x9F | (sticky << 27)
        name = f"shift_{op}_{width}_e{bits}_s{shift}_{alias}"
        if not sticky:
            name += "_qc0"
        result.append(case_type(name, (0xD51B443B,) + init + (word, 0xD53B443B) + store,
                                name, seeds, 0xB0000000, "shift"))

    immediate = {"shl": 0x5000, "sshr": 0, "ushr": 0x20000000,
                 "srshr": 0x2000, "urshr": 0x20002000,
                 "ssra": 0x1000, "usra": 0x20001000,
                 "srsra": 0x3000, "ursra": 0x20003000,
                 "sli": 0x20005000, "sri": 0x20004000}
    for width in (64, 128):
        for size, bits in enumerate((8, 16, 32, 64)):
            if width == 64 and bits == 64:
                continue
            for op, base in immediate.items():
                left = op in ("shl", "sli")
                for shift in sorted({0, 1, bits - 1} if left else {1, bits // 2, bits}):
                    imm = bits + shift if left else 2 * bits - shift
                    for alias in ("none", "src"):
                        rn = 0 if alias == "src" else 1
                        word = 0x0F000400 | ((width == 128) << 30) | (imm << 16) | base | (rn << 5)
                        add(op, width, bits, shift, alias, word)
            for op, base in (("sshl", 0x0E204400), ("ushl", 0x2E204400),
                             ("srshl", 0x0E205400), ("urshl", 0x2E205400),
                             ("sqshl", 0x0E204C00), ("uqshl", 0x2E204C00),
                             ("sqrshl", 0x0E205C00), ("uqrshl", 0x2E205C00)):
                for shift in sorted({0, 1, -1, bits, bits + 1, -bits, -bits - 1, 127, -128}):
                    for alias in ("none", "src", "count"):
                        rn = 0 if alias == "src" else 1
                        rd = 2 if alias == "count" else 0
                        word = base | ((width == 128) << 30) | (size << 22) | (2 << 16) | (rn << 5) | rd
                        add(op, width, bits, shift, alias, word, True)
                        if "q" in op:
                            add(op, width, bits, shift, alias, word, True, False)
    for bits in (8, 16, 32):
        for width in (64, 128):
            for op, base in (("sshll", 0xA000), ("ushll", 0x2000A000)):
                for shift in sorted({0, 1, bits - 1}):
                    for alias in ("none", "src"):
                        word = 0x0F000400 | ((width == 128) << 30) | ((bits + shift) << 16) | base
                        word |= (0 if alias == "src" else 1) << 5
                        add(op, width, bits, shift, alias, word)
            for shift in sorted({1, bits // 2, bits}):
                for alias in ("none", "src"):
                    word = 0x0F000400 | ((width == 128) << 30) | ((2 * bits - shift) << 16) | 0x8800
                    word |= (0 if alias == "src" else 1) << 5
                    add("rshrn", width, bits, shift, alias, word)
    return result


def expected_memory(case):
    match = re.fullmatch(r"shift_([a-z]+)_(64|128)_e(8|16|32|64)_s(-?\d+)_(none|src|count)(?:_qc0)?", case.name)
    if not match:
        return None
    op, width, bits, shift, alias = match.groups()
    width, bits, shift = int(width), int(bits), int(shift)
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    source = vectors[0 if alias == "src" else 1]
    old = vectors[0]
    mask = (1 << bits) - 1
    if op in ("sshll", "ushll"):
        values = lanes(source[8:] if width == 128 else source[:8], bits, op == "sshll")
        output = pack([v << shift for v in values], bits * 2)
    elif op == "rshrn":
        values = lanes(source, bits * 2)
        output = pack([(v + (1 << (shift - 1))) >> shift for v in values], bits)
        output = old[:8] + output if width == 128 else output.ljust(16, b"\0")
    else:
        values = lanes(source[:width // 8], bits, op.startswith("s") and op not in ("shl", "sli", "sri"))
        old_values = lanes(old[:width // 8], bits)
        out = []
        for index, value in enumerate(values):
            if op in ("sshl", "ushl", "srshl", "urshl", "sqshl", "uqshl", "sqrshl", "uqrshl"):
                count = lanes(vectors[2][:width // 8], bits)[index] & 255
                count = count - 256 if count & 128 else count
                rounded = "r" in op
                v = value << count if count >= 0 else (value + ((1 << (-count - 1)) if rounded else 0)) >> -count
                if "q" in op and count >= 0:
                    minimum, maximum = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if op.startswith("s") else (0, mask)
                    v = max(minimum, min(v, maximum))
            elif op == "shl":
                v = value << shift
            elif op == "sli":
                v = (old_values[index] & ((1 << shift) - 1)) | (value << shift)
            elif op == "sri":
                preserve = mask ^ ((1 << (bits - shift)) - 1)
                v = (old_values[index] & preserve) | (value >> shift)
            else:
                rounded = op in ("srshr", "urshr", "srsra", "ursra")
                v = (value + ((1 << (shift - 1)) if rounded else 0)) >> shift
                if op in ("ssra", "usra", "srsra", "ursra"):
                    v += old_values[index]
            out.append(v)
        output = pack(out, bits).ljust(16, b"\0")
    vectors[2 if alias == "count" else 0] = output
    return b"".join(vectors)


def expected_fpsr(case):
    fpsr = case.seeds[27]
    match = re.fullmatch(r"shift_([su]q(?:r)?shl)_(64|128)_e(8|16|32|64)_s(-?\d+)_(none|src|count)(?:_qc0)?", case.name)
    if not match:
        return fpsr
    op, width, bits, shift, alias = match.groups()
    width, bits, shift = int(width), int(bits), int(shift)
    if shift < 0:
        return fpsr
    first = 1 if alias == "src" else 3
    vector = struct.pack("<QQ", case.seeds[first], case.seeds[first + 1])[:width // 8]
    values = lanes(vector, bits, op.startswith("s"))
    minimum, maximum = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if op.startswith("s") else (0, (1 << bits) - 1)
    if any(not minimum <= value << shift <= maximum for value in values):
        fpsr |= 1 << 27
    return fpsr
