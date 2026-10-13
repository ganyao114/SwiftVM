"""FlagM RMIF/SETF8/SETF16 inputs with an independent NZCV oracle.

Use ``python3 tools/arm64_flagm_corpus.py --verify`` to validate only these
references in Capstone and Unicorn. No native VM runner is required.
"""

import argparse
import struct


MASK64 = (1 << 64) - 1


def corpus(case_type):
    result = []

    def add(name, word, source, old_flags):
        result.append(case_type(name, (word,), name, {1: source}, old_flags << 28, "flagm"))

    pattern = 0xD6A91C3F05E872B4
    for rotation in range(64):
        for mask in range(16):
            old = (rotation * 3 + mask * 7) & 15
            name = f"flagm_rmif_r{rotation}_m{mask:x}_f{old:x}"
            add(name, 0xBA000400 | (rotation << 15) | (1 << 5) | mask, pattern, old)
    # The non-rotating form exercises every input combination of old flags
    # and write mask. The source varies to oppose retained flags.
    for old in range(16):
        for mask in range(16):
            name = f"flagm_rmif_r0_m{mask:x}_f{old:x}_allflags"
            source = (pattern & ~15) | (old ^ 15)
            add(name, 0xBA000400 | (1 << 5) | mask, source, old)
    for rotation in (0, 1, 31, 32, 63):
        for mask in range(16):
            old = (rotation + mask * 7) & 15
            name = f"flagm_rmif_r{rotation}_m{mask:x}_f{old:x}_zr"
            add(name, 0xBA000400 | (rotation << 15) | (31 << 5) | mask, MASK64, old)

    # The entire nine-bit SETF8 domain covers every N/Z/V outcome. Bits above
    # bit eight deliberately remain nonzero, including the high X-register half.
    for value in range(512):
        for carry in (0, 1):
            old = ((value * 5) & 13) | (carry << 1)
            source = 0xFEDCBA98ABCD0000 | value
            name = f"flagm_setf8_v{value:x}_f{old:x}"
            add(name, 0x3A00080D | (1 << 5), source, old)
    values = {0, 1, 2, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF,
              0x10000, 0x10001, 0x17FFE, 0x17FFF, 0x18000, 0x18001, 0x1FFFE, 0x1FFFF}
    for bit in range(17):
        values.update(((1 << bit) - 1, 1 << bit, (1 << bit) + 1))
    for value in sorted(values):
        for poison in (0, 0xFEDCBA98ABFE0000):
            for carry in (0, 1):
                old = ((value * 7) & 13) | (carry << 1)
                name = f"flagm_setf16_v{value:x}_f{old:x}_hi{int(bool(poison))}"
                add(name, 0x3A00480D | (1 << 5), poison | value, old)
    for bits in (8, 16):
        for old in range(16):
            name = f"flagm_setf{bits}_zr_f{old:x}"
            add(name, (0x3A00080D if bits == 8 else 0x3A00480D) | (31 << 5), MASK64, old)
    return result


def expected_nzcv(case):
    """Return architectural NZCV without reproducing the frontend's IR path."""
    if case.family != "flagm" or len(case.instructions) != 1:
        raise ValueError(f"Unrecognized FlagM case: {case.name}")
    word = case.instructions[0]
    rn = (word >> 5) & 31
    source = 0 if rn == 31 else case.seeds[rn]
    if case.name.startswith("flagm_rmif_"):
        rotation, mask = (word >> 15) & 63, word & 15
        # Choose each architectural flag directly from its original bit
        # position; no rotation primitive is shared with the implementation.
        result = case.nzcv
        for bit in range(4):
            if mask & (1 << bit):
                output_bit = 1 << (28 + bit)
                selected = (source >> ((rotation + bit) % 64)) & 1
                result = (result & ~output_bit) | (selected << (28 + bit))
        return result
    bits = 8 if case.name.startswith("flagm_setf8_") else 16
    value = source & ((1 << bits) - 1)
    negative = value >= 1 << (bits - 1)
    zero = value == 0
    extended_negative = bool(source & (1 << bits))
    overflow = negative != extended_negative
    return ((int(negative) << 31) | (int(zero) << 30) |
            (case.nzcv & (1 << 29)) | (int(overflow) << 28))


def verify():
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as registers
    from arm64_differential import Case

    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(Case)
    if len({case.name for case in cases}) != len(cases):
        raise ValueError("Duplicate FlagM case names")
    for case in cases:
        code = struct.pack("<I", case.instructions[0])
        decoded = list(decoder.disasm(code, 0x400000))
        if len(decoded) != 1 or decoded[0].mnemonic != case.name.split("_")[1]:
            raise ValueError(f"{case.name}: independent instruction decoding failed")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(0x400000, 4096)
        cpu.mem_write(0x400000, code)
        cpu.reg_write(registers.UC_ARM64_REG_X1, case.seeds[1])
        cpu.reg_write(registers.UC_ARM64_REG_SP, MASK64 - 15)
        cpu.reg_write(registers.UC_ARM64_REG_NZCV, case.nzcv)
        cpu.emu_start(0x400000, 0x400004)
        actual, wanted = cpu.reg_read(registers.UC_ARM64_REG_NZCV), expected_nzcv(case)
        if actual != wanted:
            raise ValueError(f"{case.name}: math/Unicorn mismatch NZCV={actual:#x}, expected={wanted:#x}")
        if cpu.reg_read(registers.UC_ARM64_REG_X1) != case.seeds[1]:
            raise ValueError(f"{case.name}: source register changed")
        if cpu.reg_read(registers.UC_ARM64_REG_SP) != MASK64 - 15:
            raise ValueError(f"{case.name}: SP changed")
    print(f"PASS {len(cases)} FlagM independent math/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
