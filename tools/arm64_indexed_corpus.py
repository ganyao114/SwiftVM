"""Integer indexed SIMD multiplication with lane math and alias snapshots."""

import re
import struct
from arm64_shift_corpus import lanes, pack


def corpus(case_type):
    result = []
    bases = {"mul": 0x0F008000, "mla": 0x2F000000, "mls": 0x2F004000,
             "smull": 0x0F00A000, "smlal": 0x0F002000, "smlsl": 0x0F006000,
             "umull": 0x2F00A000, "umlal": 0x2F002000, "umlsl": 0x2F006000}
    for op, base in bases.items():
        for bits in (16, 32):
            for width in (64, 128):
                for index in range(128 // bits):
                    for alias in ("none", "src", "rhs", "all"):
                        for high_reg in (False, True) if bits == 32 and alias == "none" else (False,):
                            rm = 31 if high_reg else 15 if bits == 16 else 2
                            rn = rm if alias == "all" else 1
                            rd = rn if alias == "src" else rm if alias in ("rhs", "all") else 0
                            registers = list(dict.fromkeys((0, 1, rm)))
                            seeds, init = {}, []
                            for slot, reg in enumerate(registers):
                                values = [((1 << (bits - 1)) + (lane * 971 + slot * 331)) & ((1 << bits) - 1)
                                          for lane in range(128 // bits)]
                                data = pack(values, bits)
                                for half in range(2):
                                    gpr = slot * 2 + half + 1
                                    seeds[gpr] = int.from_bytes(data[half * 8:half * 8 + 8], "little")
                                    init.append((0x9E670000 if half == 0 else 0x9EAF0000) | (gpr << 5) | reg)
                            seeds[27] = 0x0800009F
                            word = base | ((width == 128) << 30) | ((1 if bits == 16 else 2) << 22)
                            word |= (rm & 15) << 16 | rn << 5 | rd
                            if bits == 16:
                                word |= ((index >> 2) << 11) | ((index & 1) << 20) | (((index >> 1) & 1) << 21)
                            else:
                                word |= ((rm >> 4) << 20) | ((index & 1) << 21) | ((index >> 1) << 11)
                            store_regs = [rd] + [reg for reg in registers if reg != rd]
                            stores = tuple(0x3D8003A0 | slot << 10 | reg for slot, reg in enumerate(store_regs))
                            name = f"indexed_{op}_{width}_e{bits}_i{index}_{alias}_rm{rm}"
                            result.append(case_type(name, (0xD51B443B,) + tuple(init) + (word, 0xD53B443B) + stores,
                                                    name, seeds, 0xB0000000, "indexed"))
    return result


def expected_memory(case):
    match = re.fullmatch(r"indexed_([a-z]+)_(64|128)_e(16|32)_i(\d+)_(none|src|rhs|all)_rm(\d+)", case.name)
    if not match:
        raise ValueError(f"Unrecognized indexed case: {case.name}")
    op, width, bits, index, alias, rm = match.groups()
    width, bits, index, rm = int(width), int(bits), int(index), int(rm)
    rn = rm if alias == "all" else 1
    rd = rn if alias == "src" else rm if alias in ("rhs", "all") else 0
    registers = list(dict.fromkeys((0, 1, rm)))
    vectors = {reg: struct.pack("<QQ", case.seeds[slot * 2 + 1], case.seeds[slot * 2 + 2])
               for slot, reg in enumerate(registers)}
    widening = op not in ("mul", "mla", "mls")
    signed = widening and op.startswith("s")
    source = vectors[rn]
    source = (source[8:] if width == 128 else source[:8]) if widening else source[:width // 8]
    values = lanes(source, bits, signed)
    rhs = lanes(vectors[rm], bits, signed)[index]
    destination_bits = bits * (2 if widening else 1)
    old = lanes(vectors[rd], destination_bits)
    output = []
    for lane, value in enumerate(values):
        product = value * rhs
        if op in ("mla", "smlal", "umlal"):
            product += old[lane]
        elif op in ("mls", "smlsl", "umlsl"):
            product = old[lane] - product
        output.append(product)
    vectors[rd] = pack(output, destination_bits).ljust(16, b"\0")
    order = [rd] + [reg for reg in registers if reg != rd]
    return b"".join(vectors[reg] for reg in order)
