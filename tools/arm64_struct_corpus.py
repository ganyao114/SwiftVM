"""Unaligned multi-register structure transfers and independent byte-order math."""

import re
import struct

FIXTURE = bytes((n * 37 + 11) & 255 for n in range(64))


def corpus(case_type):
    cases = []
    for kind, count, opcode in (("ld1x1", 1, 7), ("ld1x2", 2, 10), ("ld1x3", 3, 6),
                                ("ld1x4", 4, 2), ("ld2", 2, 8), ("ld3", 3, 4), ("ld4", 4, 0)):
        for load in (False, True):
            op = kind if load else "st" + kind[2:]
            for width in (64, 128):
                for size, bits in enumerate((8, 16, 32, 64)):
                    if width == 64 and bits == 64 and "x" not in kind:
                        continue
                    for rt in (0, 30):
                        for post in ("none", "imm", "reg", "alias"):
                            seeds = {n: (0x1020304050607080 + n * 0x111111111111111) & ((1 << 64) - 1)
                                     for n in range(1, 9)}
                            seeds[9] = 17
                            init = tuple(word for index in range(4) for word in
                                         (0x9E670000 | ((index * 2 + 1) << 5) | ((rt + index) & 31),
                                          0x9EAF0000 | ((index * 2 + 2) << 5) | ((rt + index) & 31)))
                            word = 0x0C000000 | (load << 22) | ((width == 128) << 30) | (size << 10) | (opcode << 12) | rt
                            if post != "none":
                                word |= 1 << 23
                                word |= {"imm": 31, "reg": 9, "alias": 0}[post] << 16
                            total = count * width // 8
                            # Both X0 and X26 point one byte into the fixture; preserve X26 across writeback.
                            instructions = init + (0x910007A0, 0x910007BA, word,
                                                   0x39400000 | ((total - 1) << 10) | (26 << 5) | 27,
                                                   0xCB1A0000)
                            if load:
                                instructions += tuple(0x3D8003A0 | (index << 10) | ((rt + index) & 31)
                                                      for index in range(4))
                            name = f"struct_{op}_{width}_e{bits}_v{rt}_{post}"
                            cases.append(case_type(name, instructions, name, seeds, 0xB0000000,
                                                   "struct", FIXTURE))
    return cases


def expected(case, memory_base):
    match = re.fullmatch(r"struct_([a-z0-9]+)_(64|128)_e(8|16|32|64)_v(0|30)_(none|imm|reg|alias)", case.name)
    if not match:
        raise ValueError(f"Unrecognized structure case: {case.name}")
    op, width, bits, _, post = match.groups()
    width, bits = int(width), int(bits)
    load, contiguous = op.startswith("ld"), "x" in op
    count = int(op[-1])
    vector_bytes, lane_bytes = width // 8, bits // 8
    total = count * vector_bytes
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    memory = bytearray(FIXTURE + b"\0")
    if load:
        source = bytes(memory[1:total + 1])
        for reg in range(count):
            vector = source[reg * vector_bytes:(reg + 1) * vector_bytes] if contiguous else b"".join(
                source[(lane * count + reg) * lane_bytes:(lane * count + reg + 1) * lane_bytes]
                for lane in range(vector_bytes // lane_bytes))
            vectors[reg] = vector.ljust(16, b"\0")
        last = source[-1]
        output = b"".join(vectors)
    else:
        source = b"".join(vector[:vector_bytes] for vector in vectors[:count]) if contiguous else b"".join(
            vectors[reg][lane * lane_bytes:(lane + 1) * lane_bytes]
            for lane in range(vector_bytes // lane_bytes) for reg in range(count))
        memory[1:total + 1] = source
        last, output = source[-1], bytes(memory[:64])
    increment = {"none": 0, "imm": total, "reg": 17, "alias": memory_base + 1}[post]
    return {"memory": output, "x0": increment, "x27": last}
