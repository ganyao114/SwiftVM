"""Unsigned fixed-point reciprocal estimates with independent rational roots."""
import argparse
import math
import random
import struct
from fractions import Fraction

OPS = ("urecpe", "ursqrte")
ALIASES = ((0, 1), (1, 1), (2, 1))
CONTROLS = (0, 1 << 22, 2 << 22, 3 << 22, 1 << 19, 1 << 24, 1 << 25,
            (1 << 19) | (1 << 24) | (1 << 25))
EDGES = (0, 1, 0x3fffffff, 0x40000000, 0x40000001, 0x7fffffff,
         0x80000000, 0x80000001, 0xfffffffe, 0xffffffff, 0x40800000,
         0x80800000, 0xc0000000, 0x7f800000, 0xff800000, 0xff000000)


def reference(value, operation):
    if value < (0x80000000 if operation == 0 else 0x40000000):
        return 0xffffffff
    shift = 23 if operation == 0 or value < 0x80000000 else 24
    midpoint = Fraction(2 * (value >> shift) + 1, 1 << (33 - shift))
    if operation == 0:
        estimate = round(256 / midpoint)
    else:
        squared = 65536 / midpoint
        estimate = math.isqrt(squared.numerator // squared.denominator)
        if Fraction((2 * estimate + 1) ** 2, 4) < squared:
            estimate += 1
    return estimate << 23


def corpus(case_type, family=None):
    cases, rng = [], random.Random(0xA64E2026)
    physical = (0, 31, 30, 3)
    for operation, op in enumerate(OPS):
        if family is not None and family != op:
            continue
        for q in (0, 1):
            for alias, (rd, rn) in enumerate(ALIASES):
                for control, fpcr in enumerate(CONTROLS):
                    for profile in range(96):
                        vectors = [rng.getrandbits(128) for _ in physical]
                        lanes = [rng.getrandbits(32) for _ in range(4)]
                        if profile < 16:
                            lanes = [EDGES[(profile + lane) % len(EDGES)] for lane in range(4)]
                        elif profile < 80:
                            lanes = [((128 + (profile - 16) * 6 + lane) << 23) & 0xffffffff
                                     for lane in range(4)]
                        vectors[rn] = sum(value << (32 * lane) for lane, value in enumerate(lanes))
                        seeds = {1 + slot * 2 + part: (vector >> (64 * part)) & ((1 << 64) - 1)
                                 for slot, vector in enumerate(vectors) for part in range(2)}
                        seeds.update({27: 0x0800009f, 28: fpcr})
                        initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                           (0x9e670000 | ((slot * 2 + 1) << 5) | reg,
                                            0x9eaf0000 | ((slot * 2 + 2) << 5) | reg))
                        word = (0x0ea1c800 if operation == 0 else 0x2ea1c800) | (q << 30)
                        word |= (physical[rn] << 5) | physical[rd]
                        capture = (0xd53b443b, 0xd53b441c) + tuple(
                            0x3d8003a0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                        name = f"unsignedestimate_{op}_q{q}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name, (0xd51b443b, 0xd51b441c) + initialize +
                                               (word,) + capture, name, seeds, 0xb0000000,
                                               "unsignedestimate", bytes(range(64))))
    if len(cases) != (9216 if family is None else 4608):
        raise ValueError("Unsigned estimate coverage changed")
    return cases


def expected(case):
    import arm64_differential as differential
    _, op, qtext, aliastext, _, _ = case.name.split("_")
    q, operation = int(qtext[1:]), OPS.index(op)
    rd, rn = ALIASES[int(aliastext[1:])]
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]
    source = vectors[rn]
    vectors[rd] = sum(reference((source >> (32 * lane)) & 0xffffffff, operation) << (32 * lane)
                      for lane in range(4 if q else 2))
    registers = {f"x{n}": case.seeds.get(n, 0x100 + n) for n in range(29)}
    registers.update({"x29": differential.MEMORY_BASE, "x30": differential.DATA_BASE})
    return {"memory": b"".join(vector.to_bytes(16, "little") for vector in vectors),
            "registers": registers, "nzcv": case.nzcv, "fpsr": case.seeds[27], "fpcr": case.seeds[28]}


def verify(family=None):
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    import unicorn.arm64_const as r
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case, family)
    for case in cases:
        elf = differential.make_elf(case)
        code = struct.unpack_from("<IIQQQQQQ", elf, 64)
        data = struct.unpack_from("<IIQQQQQQ", elf, 120)
        instructions = list(decoder.disasm(elf[4096:code[5]], differential.CODE_BASE + 4096))
        if len(instructions) * 4 != code[5] - 4096 or sum(
                instruction.mnemonic == case.name.split("_")[1] for instruction in instructions) != 1:
            raise ValueError(f"{case.name}: instruction decoding differs")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
        cpu.mem_map(differential.CODE_BASE, 65536)
        cpu.mem_map(differential.DATA_BASE, 65536)
        cpu.mem_write(differential.CODE_BASE, elf[:code[5]])
        cpu.mem_write(differential.DATA_BASE, elf[data[2]:data[2] + data[5]])
        snapshots, exited = [], []

        def interrupt(emulator, number, _):
            syscall = emulator.reg_read(r.UC_ARM64_REG_X8)
            if syscall == 64:
                if emulator.reg_read(r.UC_ARM64_REG_X0) != 1:
                    raise ValueError("Harness write descriptor differs")
                snapshots.append(bytes(emulator.mem_read(emulator.reg_read(r.UC_ARM64_REG_X1),
                                                        emulator.reg_read(r.UC_ARM64_REG_X2))))
                emulator.reg_write(r.UC_ARM64_REG_X0, differential.SNAPSHOT_SIZE)
            elif syscall == 93 and emulator.reg_read(r.UC_ARM64_REG_X0) == 0:
                exited.append(True)
                emulator.emu_stop()
            else:
                raise ValueError(f"Unexpected interrupt {number}, syscall {syscall}")

        cpu.hook_add(UC_HOOK_INTR, interrupt)
        cpu.emu_start(differential.CODE_BASE + 4096, 0, count=2000)
        if len(snapshots) != 1 or not exited:
            raise ValueError(f"{case.name}: incomplete harness execution")
        actual, wanted = differential.decode_snapshot(snapshots[0], case), expected(case)
        if bytes.fromhex(actual["memory"]) != wanted["memory"] or int(actual["nzcv"], 16) != wanted["nzcv"]:
            raise ValueError(f"{case.name}: independent byte or flag reference differs")
        for reg, value in wanted["registers"].items():
            if int(actual["registers"][reg], 16) != value:
                raise ValueError(f"{case.name}: register {reg} differs")
    print(f"PASS {len(cases)} unsigned estimates / fixed-point rational roots / full ELF / Capstone / Unicorn")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true", required=True)
    parser.add_argument("--family", choices=OPS)
    verify(parser.parse_args().family)
