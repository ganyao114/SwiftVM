"""FP16 vector FSQRT/FRINT: integer root and rounding oracle, full ELF snapshots."""

import argparse
import math
import random
import struct


OPS = ("fsqrt", "frintn", "frintm", "frintp", "frintz", "frinta", "frintx", "frinti")
BASES = (0x2EF9F800, 0x0E798800, 0x0E799800, 0x0EF98800, 0x0EF99800,
         0x2E798800, 0x2E799800, 0x2EF99800)
ALIASES = ((0, 1), (1, 1), (2, 1))
CONTROLS = (0, 1 << 22, 2 << 22, 3 << 22, 1 << 24, 1 << 25,
            (1 << 19) | (1 << 25), 1 << 19)
MASK64 = (1 << 64) - 1


def reference(value, op, fpcr):
    sign, exponent, fraction = value & 0x8000, (value >> 10) & 31, value & 1023
    if exponent == 31 and fraction:
        flags = 0 if fraction & 512 else 1
        return 0x7E00 if fpcr & (1 << 25) else value | 512, flags
    if not exponent and fraction and fpcr & (1 << 19):
        value, fraction = sign, 0
    if not value & 0x7FFF:
        return value, 0
    if exponent == 31:
        return (0x7E00, 1) if sign and op == "fsqrt" else (value, 0)
    magnitude = ((fraction | 1024) << (exponent - 1)) if exponent else fraction
    mode = (fpcr >> 22) & 3
    if op == "fsqrt":
        if sign:
            return 0x7E00, 1
        target = magnitude << 24
        root = math.isqrt(target)
        shift = max(0, root.bit_length() - 1 - 10)
        rounded = root >> shift
        exact = (rounded << shift) ** 2 == target
        increment = mode == 1 and not exact
        if mode == 0:
            midpoint = ((2 * rounded + 1) << shift) ** 2
            increment = 4 * target > midpoint or (4 * target == midpoint and bool(rounded & 1))
        rounded += increment
        if rounded >= 2048:
            rounded >>= 1
            shift += 1
        return ((shift + 1) << 10) | (rounded & 1023), 0 if exact else 16
    if op == "frintn":
        mode = 0
    elif op == "frintp":
        mode = 1
    elif op == "frintm":
        mode = 2
    elif op == "frintz":
        mode = 3
    elif op == "frinta":
        mode = 4
    rounded, remainder = divmod(magnitude, 1 << 24)
    if remainder:
        increment = (mode == 1 and not sign) or (mode == 2 and bool(sign))
        if mode in (0, 4):
            increment = remainder > 1 << 23 or (remainder == 1 << 23 and (mode == 4 or bool(rounded & 1)))
        rounded += increment
    flags = 16 if remainder and op == "frintx" else 0
    if not rounded:
        return sign, flags
    top = rounded.bit_length() - 1
    significand = (rounded << 10) >> top
    return sign | ((top + 15) << 10) | (significand & 1023), flags


def corpus(case_type):
    rng, cases, physical = random.Random(0xF01601A42026), [], (0, 31, 30, 3)
    profiles = (
        (0x0000, 0x8000, 0x3C00, 0x4000, 0x4200, 0x4400, 0x7C00, 0xFC00),
        (0x3800, 0xB800, 0x3E00, 0xBE00, 0x4100, 0xC100, 0x4300, 0xC300),
        (0x37FF, 0x3801, 0xB7FF, 0xB801, 0x3DFF, 0x3E01, 0xBDFF, 0xBE01),
        (0x0001, 0x8001, 0x03FF, 0x83FF, 0x0400, 0x8400, 0x0401, 0x8401),
        (0x7E03, 0xFE05, 0x7C01, 0xFC03, 0x7FFF, 0xFFFF, 0x7DFF, 0xFDFF),
        (0x3C00, 0x4000, 0x4400, 0x4800, 0x7C01, 0xBC00, 0x7DFF, 0xFC00),
        (0x3555, 0x3955, 0x3C01, 0x3BFF, 0x7BFF, 0x7BFE, 0x5801, 0xD801),
    )
    for op_index, op in enumerate(OPS):
        for q in (0, 1):
            for alias, (rd, rn) in enumerate(ALIASES):
                for control, fpcr in enumerate(CONTROLS):
                    for profile in range(8):
                        vectors = [rng.getrandbits(128) for _ in range(4)]
                        values = profiles[profile] if profile < 7 else tuple(rng.getrandbits(16) for _ in range(8))
                        vectors[rn] = sum(value << (16 * lane) for lane, value in enumerate(values))
                        seeds = {1 + slot * 2 + part: (vector >> (64 * part)) & MASK64
                                 for slot, vector in enumerate(vectors) for part in range(2)}
                        seeds.update({27: 0x08000002, 28: fpcr})
                        initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                           (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                            0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                        word = BASES[op_index] | (q << 30) | (physical[rn] << 5) | physical[rd]
                        capture = (0xD53B443B, 0xD53B441C) + tuple(
                            0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                        name = f"fp16unary_{op}_q{q}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                                               name, seeds, 0xB0000000, "fp16unary", bytes(range(64))))
    if len(cases) != 3072 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"FP16 unary coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, op, q_text, alias_text, _, _ = case.name.split("_")
    q, alias = int(q_text[1:]), int(alias_text[1:])
    rd, rn = ALIASES[alias]
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]
    source, result, fpsr = vectors[rn], 0, case.seeds[27]
    for lane in range(8 if q else 4):
        value, flags = reference((source >> (lane * 16)) & 65535, op, case.seeds[28])
        result |= value << (lane * 16)
        fpsr |= flags
    vectors[rd] = result
    registers = {f"x{n}": case.seeds.get(n, 0x100 + n) for n in range(29)}
    registers.update({"x27": fpsr, "x28": case.seeds[28],
                      "x29": differential.MEMORY_BASE, "x30": differential.DATA_BASE})
    return {"memory": b"".join(value.to_bytes(16, "little") for value in vectors),
            "nzcv": case.nzcv, "registers": registers, "fpsr": fpsr, "fpcr": case.seeds[28]}


def verify():
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case)
    for case in cases:
        elf = differential.make_elf(case)
        code_segment = struct.unpack_from("<IIQQQQQQ", elf, 64)
        data_segment = struct.unpack_from("<IIQQQQQQ", elf, 120)
        code = elf[4096:code_segment[5]]
        decoded = list(decoder.disasm(code, differential.CODE_BASE + 4096))
        if len(decoded) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected ELF harness code")
        op = case.name.split("_")[1]
        if sum(instruction.mnemonic == op for instruction in decoded) != 1:
            raise ValueError(f"{case.name}: Capstone did not decode the intended operation")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(differential.CODE_BASE, 65536)
        cpu.mem_map(differential.DATA_BASE, 65536)
        cpu.mem_write(differential.CODE_BASE, elf[:code_segment[5]])
        cpu.mem_write(differential.DATA_BASE, elf[data_segment[2]:data_segment[2] + data_segment[5]])
        snapshots, exited = [], []

        def interrupt(emulator, number, _):
            syscall = emulator.reg_read(registers.UC_ARM64_REG_X8)
            if syscall == 64:
                if emulator.reg_read(registers.UC_ARM64_REG_X0) != 1:
                    raise ValueError(f"{case.name}: write does not target stdout")
                snapshots.append(bytes(emulator.mem_read(emulator.reg_read(registers.UC_ARM64_REG_X1),
                                                        emulator.reg_read(registers.UC_ARM64_REG_X2))))
                emulator.reg_write(registers.UC_ARM64_REG_X0, differential.SNAPSHOT_SIZE)
            elif syscall == 93:
                if emulator.reg_read(registers.UC_ARM64_REG_X0) != 0:
                    raise ValueError(f"{case.name}: harness error exit")
                exited.append(True)
                emulator.emu_stop()
            else:
                raise ValueError(f"{case.name}: unexpected interrupt={number}, syscall={syscall}")

        cpu.hook_add(UC_HOOK_INTR, interrupt)
        cpu.emu_start(differential.CODE_BASE + 4096, 0, count=2000)
        if len(snapshots) != 1 or not exited:
            raise ValueError(f"{case.name}: missing full ELF snapshot/exit")
        state, wanted = differential.decode_snapshot(snapshots[0], case), expected(case)
        if bytes.fromhex(state["memory"]) != wanted["memory"] or int(state["nzcv"], 16) != wanted["nzcv"]:
            raise ValueError(f"{case.name}: independent FP16 integer root and rounding/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} vector FP16 unary/integer root/FRINT/NaN/FZ16/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
