"""Scalar signed doubling long multiply with an independent two-stage clamp oracle.

The bounded corpus records four complete Q registers (64 fixture bytes), the
sticky QC state, FPCR and NZCV. Standalone verification executes the same full
ELF snapshot harness used by the SwiftVM differential runner.
"""

import argparse
import random
import struct


REGISTER_BASES = {"sqdmull": 0x5E20D000, "sqdmlal": 0x5E209000, "sqdmlsl": 0x5E20B000}
INDEXED_BASES = {"sqdmull": 0x5F00B000, "sqdmlal": 0x5F003000, "sqdmlsl": 0x5F007000}
ALIASES = ((0, 1, 2), (1, 1, 2), (2, 1, 2), (0, 2, 2), (2, 2, 2))
MASK64 = (1 << 64) - 1
QC = 1 << 27


def physical_registers(bits):
    # Rm=15 exercises the halfword restriction; Rm=30 exercises the full
    # word register field. Rn=31 is a SIMD register, never a zero register.
    return (0, 31, 15 if bits == 16 else 30, 3)


def corpus(case_type):
    cases, rng = [], random.Random(0x5CA1A4D012026)
    for bits in (16, 32):
        physical = physical_registers(bits)
        sign, out_sign = 1 << (bits - 1), 1 << (bits * 2 - 1)
        profiles = [("productcancel", sign, sign, -1), ("productzero", sign, sign, 0),
                    ("productnegative", sign, sign, out_sign), ("productpositive", sign, sign, out_sign - 1),
                    ("accpositive", 1, 1, out_sign - 1), ("accnegative", 1, 1, out_sign),
                    ("negativelower", -1, 1, out_sign), ("negativeupper", -1, 1, out_sign - 1),
                    ("minmax", sign, sign - 1, out_sign), ("maxmax", sign - 1, sign - 1, 0),
                    ("zerosticky", 0, sign, -1), ("zeroclear", 0, sign, 0)]
        profiles += [(f"random{n}", rng.getrandbits(bits), rng.getrandbits(bits), rng.getrandbits(bits * 2))
                     for n in range(2)]
        for form, bases in (("reg", REGISTER_BASES), ("idx", INDEXED_BASES)):
            selected = list(range(14)) if form == "reg" else (0, 1, 4, 5, 7, 12)
            for op, base in bases.items():
                for index in range(128 // bits if form == "idx" else 1):
                    for mode, (rd, rn, rm) in enumerate(ALIASES):
                        for profile_index in selected:
                            profile, lhs, rhs, accumulator = profiles[profile_index]
                            sticky = (profile_index + index + mode) & 1
                            vectors = [rng.getrandbits(128) for _ in range(4)]
                            out_mask, source_mask = (1 << (bits * 2)) - 1, (1 << bits) - 1
                            vectors[rd] = (vectors[rd] & ~out_mask) | (accumulator & out_mask)
                            vectors[rn] = (vectors[rn] & ~source_mask) | (lhs & source_mask)
                            shift = bits * index if form == "idx" else 0
                            vectors[rm] = (vectors[rm] & ~(source_mask << shift)) | ((rhs & source_mask) << shift)
                            seeds = {1 + 2 * slot + half: (vector >> (64 * half)) & MASK64
                                     for slot, vector in enumerate(vectors) for half in range(2)}
                            seeds.update({27: 0x9F | (QC if sticky else 0), 28: 0x01400000})
                            initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                               (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                                0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                            size = 1 if bits == 16 else 2
                            word = base | (size << 22) | (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                            if form == "idx":
                                word |= ((index >> (2 if bits == 16 else 1)) & 1) << 11  # H.
                                word |= ((index >> (1 if bits == 16 else 0)) & 1) << 21  # L.
                                if bits == 16:
                                    word |= (index & 1) << 20  # M is part of the index for H.
                            capture = (0xD53B443B, 0xD53B441C) + tuple(
                                0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                            name = f"scalarmul_{form}_{op}_e{bits}_i{index}_a{mode}_{profile}_qc{sticky}"
                            words = (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture
                            fixture = bytes((0x93 + n * 13) & 255 for n in range(64))
                            cases.append(case_type(name, words, name, seeds, 0xB0000000, "scalarmul", fixture))
    if len(cases) != 1500 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"Scalar multiply coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, form, op, width, index_text, alias_text, _, _ = case.name.split("_")
    bits, index, mode = int(width[1:]), int(index_text[1:]), int(alias_text[1:])
    rd, rn, rm = ALIASES[mode]
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]

    def signed(raw, width):
        value = raw & ((1 << width) - 1)
        return value - (1 << width) if value & (1 << (width - 1)) else value

    lower, upper = -(1 << (bits * 2 - 1)), (1 << (bits * 2 - 1)) - 1
    clamp = lambda value: max(lower, min(upper, value))
    lhs = signed(vectors[rn], bits)
    rhs = signed(vectors[rm] >> (bits * index if form == "idx" else 0), bits)
    product = 2 * lhs * rhs
    clamped_product = clamp(product)
    accumulator = signed(vectors[rd], bits * 2)
    exact = clamped_product
    if op == "sqdmlal":
        exact = accumulator + clamped_product
    elif op == "sqdmlsl":
        exact = accumulator - clamped_product
    result = clamp(exact)
    saturated = product != clamped_product or exact != result
    vectors[rd] = result & ((1 << (bits * 2)) - 1)  # All inactive scalar bits clear.
    fpsr = case.seeds[27] | (QC if saturated else 0)
    registers = {f"x{n}": case.seeds.get(n, 0x100 + n) for n in range(29)}
    registers.update({"x27": fpsr, "x28": case.seeds[28],
                      "x29": differential.MEMORY_BASE, "x30": differential.DATA_BASE})
    return {"memory": b"".join(value.to_bytes(16, "little") for value in vectors),
            "fpsr": fpsr, "fpcr": case.seeds[28], "nzcv": case.nzcv, "registers": registers}


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
        if len(list(decoder.disasm(code, differential.CODE_BASE + 4096))) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected ELF harness code")
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
                    raise ValueError(f"{case.name}: harness write does not target stdout")
                snapshots.append(bytes(emulator.mem_read(emulator.reg_read(registers.UC_ARM64_REG_X1),
                                                        emulator.reg_read(registers.UC_ARM64_REG_X2))))
                emulator.reg_write(registers.UC_ARM64_REG_X0, differential.SNAPSHOT_SIZE)
            elif syscall == 93:
                if emulator.reg_read(registers.UC_ARM64_REG_X0) != 0:
                    raise ValueError(f"{case.name}: harness exited with an error")
                exited.append(True)
                emulator.emu_stop()
            else:
                raise ValueError(f"{case.name}: unexpected interrupt={number}, syscall={syscall}")

        cpu.hook_add(UC_HOOK_INTR, interrupt)
        cpu.emu_start(differential.CODE_BASE + 4096, 0, count=2000)
        if len(snapshots) != 1 or not exited:
            raise ValueError(f"{case.name}: missing full ELF snapshot/exit")
        state = differential.decode_snapshot(snapshots[0], case)
        wanted = expected(case)
        if bytes.fromhex(state["memory"]) != wanted["memory"] or int(state["nzcv"], 16) != wanted["nzcv"]:
            raise ValueError(f"{case.name}: independent two-stage clamp/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} independent reference differs from Unicorn")
    print(f"PASS {len(cases)} scalar multiply integer/two-stage saturation/QC/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
