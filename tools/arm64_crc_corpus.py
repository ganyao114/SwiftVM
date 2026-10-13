"""CRC32/CRC32C differential inputs with full polynomial long-division oracle."""

import argparse
import random


MASK64 = (1 << 64) - 1
BASES = {(castagnoli, width): 0x1AC00000 | (0x5000 if castagnoli else 0x4000)
         | (index << 10) | (0x80000000 if width == 64 else 0)
         for castagnoli in (False, True) for index, width in enumerate((8, 16, 32, 64))}


def reverse(value, width):
    return int(f"{value & ((1 << width) - 1):0{width}b}"[::-1], 2)


def reference(accumulator, data, width, castagnoli):
    # Form the entire dividend first, then divide normal polynomials. This is
    # independent of reflected table lookup and iterative CRC update loops.
    dividend = (reverse(accumulator, 32) << width) ^ (reverse(data, width) << 32)
    polynomial = (1 << 32) | (0x1EDC6F41 if castagnoli else 0x04C11DB7)
    while dividend.bit_length() > 32:
        dividend ^= polynomial << (dividend.bit_length() - 33)
    return reverse(dividend, 32)


def corpus(case_type):
    cases = []
    control_init = (0xD51B443B, 0xD51B441C)
    control_read = (0xD53B443B, 0xD53B441C)
    aliases = ((0, 1, 2), (1, 1, 2), (2, 1, 2), (1, 1, 1), (0, 1, 1),
               (0, 31, 2), (0, 1, 31), (31, 1, 2), (31, 31, 31))
    rng = random.Random(0xC2C32026)
    for (castagnoli, width), base in BASES.items():
        values = list(dict.fromkeys((0, 1, 0x80, 0xFF, 0x8000, 0xFFFF, 0x80000000,
                                    0xFFFFFFFF, 1 << (width - 1), (1 << width) - 1,
                                    0xFEDCBA9876543210)))
        values += [rng.getrandbits(64) for _ in range(12)]
        for profile, data in enumerate(values):
            for accumulator in (0, 1, 0x80000000, 0xFFFFFFFF, 0x12345678):
                for rd, rn, rm in aliases:
                    seeds = {0: MASK64, 27: 0x0800009F, 28: 0x01400000}
                    if rn != 31:
                        seeds[rn] = 0xC0FFEE5500000000 | accumulator
                    if rm != 31:
                        seeds[rm] = data if width == 64 else 0xDEADBEEF00000000 | (data & 0xFFFFFFFF)
                    word = base | (rm << 16) | (rn << 5) | rd
                    name = f"crc_{'c' if castagnoli else 'i'}_e{width}_p{profile}_a{accumulator:x}_r{rd}_{rn}_{rm}"
                    cases.append(case_type(name, control_init + (word,) + control_read,
                                           name, seeds, 0xB0000000, "crc"))
        for value in range(256) if width == 8 else ():
            seeds = {0: MASK64, 1: 0xBADC0DE012345678, 2: 0xDEADBEEFFF00FF00 | value,
                     27: 0x0800009F, 28: 0x01400000}
            name = f"crc_{'c' if castagnoli else 'i'}_byte{value:02x}_poison"
            cases.append(case_type(name, control_init + (base | (2 << 16) | (1 << 5),) + control_read,
                                   name, seeds, 0xB0000000, "crc"))

    for castagnoli in (False, True):
        for chunk in (1, 2, 4, 8):
            seeds = {0: 0xDEADBEEFFFFFFFFF, 27: 0x0800009F, 28: 0x01400000}
            message, offset, source, words = b"123456789", 0, 1, []
            while offset < len(message):
                count = chunk if offset + chunk <= len(message) else 1
                seeds[source] = int.from_bytes(message[offset:offset + count], "little")
                words.append(BASES[castagnoli, count * 8] | (source << 16))
                offset += count
                source += 1
            name = f"crc_check_{'c' if castagnoli else 'i'}_chunk{chunk}"
            cases.append(case_type(name, control_init + tuple(words) + control_read,
                                   name, seeds, 0xB0000000, "crc"))
    return cases


def expected_registers(case):
    registers = {f"x{n}": case.seeds.get(n, 0x100 + n) for n in range(29)}
    for word in case.instructions:
        if (word & 0x7FE0C000) != 0x1AC04000:
            continue
        rd, rn, rm = word & 31, (word >> 5) & 31, (word >> 16) & 31
        width = 8 << ((word >> 10) & 3)
        castagnoli = bool(word & 0x1000)
        accumulator = 0 if rn == 31 else registers[f"x{rn}"]
        data = 0 if rm == 31 else registers[f"x{rm}"]
        if rd != 31:
            registers[f"x{rd}"] = reference(accumulator, data, width, castagnoli)
    if case.name.startswith("crc_check_"):
        wanted = (0xE3069283 if "_c_" in case.name else 0xCBF43926) ^ 0xFFFFFFFF
        if registers["x0"] != wanted:
            raise ValueError(f"{case.name}: oracle disagrees with published CRC check value")
    return registers


def verify():
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case)
    for case in cases:
        words = case.instructions
        code = b"".join(word.to_bytes(4, "little") for word in words)
        if len(list(decoder.disasm(code, 0x400000))) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected code")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(0x400000, 4096)
        cpu.mem_write(0x400000, code)
        ids = [getattr(registers, f"UC_ARM64_REG_X{n}") for n in range(29)]
        for n, reg in enumerate(ids):
            cpu.reg_write(reg, case.seeds.get(n, 0x100 + n))
        cpu.reg_write(registers.UC_ARM64_REG_NZCV, case.nzcv)
        cpu.reg_write(registers.UC_ARM64_REG_SP, MASK64 - 15)
        cpu.emu_start(0x400000, 0x400000 + len(code))
        expected = expected_registers(case)
        for n, reg in enumerate(ids):
            if cpu.reg_read(reg) != expected[f"x{n}"]:
                raise ValueError(f"{case.name}: x{n} math/Unicorn disagreement")
        if (cpu.reg_read(registers.UC_ARM64_REG_NZCV) != case.nzcv or
                cpu.reg_read(registers.UC_ARM64_REG_SP) != MASK64 - 15):
            raise ValueError(f"{case.name}: NZCV/SP changed")
    print(f"PASS {len(cases)} CRC independent polynomial-division/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
