"""Single-lane structure transfers and replicate loads, with byte-level math."""

import re
import struct
from arm64_struct_corpus import FIXTURE


def corpus(case_type):
    result = []
    for count in range(1, 5):
        for size, bits in enumerate((8, 16, 32, 64)):
            for operation in ("load", "store", "replicate"):
                indices = [(width, None) for width in (64, 128)] if operation == "replicate" else [(128, index) for index in range(128 // bits)]
                for width, index in indices:
                    for rt in (0, 30):
                        for post in ("none", "imm", "reg", "alias"):
                            seeds = {n: (0x1122334455667788 + n * 0x0102040810204080) & ((1 << 64) - 1) for n in range(1, 9)}
                            seeds[9] = 19
                            init = tuple(word for n in range(4) for word in
                                         (0x9E670000 | ((n * 2 + 1) << 5) | ((rt + n) & 31),
                                          0x9EAF0000 | ((n * 2 + 2) << 5) | ((rt + n) & 31)))
                            word = 0x0D000000 | ((operation != "store") << 22) | (((count - 1) & 1) << 21)
                            word |= (((count - 1) >> 1) << 13) | rt
                            if operation == "replicate":
                                word |= 3 << 14 | size << 10 | ((width == 128) << 30)
                            else:
                                composite = index << size
                                scale = min(size, 2)
                                word |= scale << 14 | ((composite >> 3) << 30) | (((composite >> 2) & 1) << 12)
                                word |= (composite & 3) << 10
                                if size == 3:
                                    word |= 1 << 10
                            if post != "none":
                                word |= 1 << 23 | {"imm": 31, "reg": 9, "alias": 0}[post] << 16
                            instructions = init + (0x910007A0, 0x910007BA, word, 0xCB1A0000)
                            if operation != "store":
                                instructions += tuple(0x3D8003A0 | (n << 10) | ((rt + n) & 31) for n in range(4))
                            label = f"q{width}" if operation == "replicate" else f"lane{index}"
                            name = f"single_{operation}_c{count}_e{bits}_{label}_v{rt}_{post}"
                            result.append(case_type(name, instructions, name, seeds, 0xB0000000, "single", FIXTURE))
    return result


def expected(case, memory_base):
    match = re.fullmatch(r"single_(load|store|replicate)_c([1-4])_e(8|16|32|64)_(lane\d+|q64|q128)_v(0|30)_(none|imm|reg|alias)", case.name)
    if not match:
        raise ValueError(f"Unrecognized single memory case: {case.name}")
    operation, count, bits, label, _, post = match.groups()
    count, bits = int(count), int(bits)
    lane_bytes = bits // 8
    vectors = [bytearray(struct.pack("<QQ", case.seeds[n], case.seeds[n + 1])) for n in (1, 3, 5, 7)]
    memory = bytearray(FIXTURE)
    for reg in range(count):
        memory_index = 1 + reg * lane_bytes
        if operation == "replicate":
            width = int(label[1:])
            vectors[reg] = bytearray((memory[memory_index:memory_index + lane_bytes] * (width // bits)).ljust(16, b"\0"))
        else:
            index = int(label[4:]) * lane_bytes
            if operation == "load":
                vectors[reg][index:index + lane_bytes] = memory[memory_index:memory_index + lane_bytes]
            else:
                memory[memory_index:memory_index + lane_bytes] = vectors[reg][index:index + lane_bytes]
    increment = {"none": 0, "imm": count * lane_bytes, "reg": 19, "alias": memory_base + 1}[post]
    return {"memory": bytes(memory) if operation == "store" else b"".join(vectors), "x0": increment}
