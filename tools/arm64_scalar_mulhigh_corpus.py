"""Scalar SQDMULH/SQRDMULH: exact Python integers and complete ELF snapshots."""

import argparse
import random
import struct


ALIASES = ((0, 1, 2), (1, 1, 2), (2, 1, 2), (0, 2, 2), (2, 2, 2))
QC = 1 << 27
MASK64 = (1 << 64) - 1


def corpus(case_type):
    cases, rng = [], random.Random(0x5CA1A4D012027)
    for bits in (16, 32):
        sign = 1 << (bits - 1)
        physical = (0, 31, 15 if bits == 16 else 30, 3)
        profiles = [("minmin", -sign, -sign), ("minmax", -sign, sign - 1),
                    ("maxmax", sign - 1, sign - 1), ("minone", -sign, 1),
                    ("positivehalf", 1, sign // 2), ("negativehalf", -1, sign // 2),
                    ("belowpositivehalf", 1, sign // 2 - 1),
                    ("belownegativehalf", -1, sign // 2 + 1),
                    ("zero", 0, -sign), ("negativeone", -1, 1)]
        profiles += [(f"random{n}", rng.getrandbits(bits), rng.getrandbits(bits)) for n in range(4)]
        for form in ("reg", "idx"):
            for op in ("sqdmulh", "sqrdmulh"):
                base = ({"sqdmulh": 0x5E20B400, "sqrdmulh": 0x7E20B400} if form == "reg" else
                        {"sqdmulh": 0x5F00C000, "sqrdmulh": 0x5F00D000})[op]
                for index in range(128 // bits if form == "idx" else 1):
                    for mode, (rd, rn, rm) in enumerate(ALIASES):
                        for profile_index, (profile, lhs, rhs) in enumerate(profiles):
                            sticky = (profile_index + index + mode) & 1
                            vectors = [rng.getrandbits(128) for _ in range(4)]
                            mask, shift = (1 << bits) - 1, bits * index if form == "idx" else 0
                            vectors[rn] = (vectors[rn] & ~mask) | (lhs & mask)
                            vectors[rm] = (vectors[rm] & ~(mask << shift)) | ((rhs & mask) << shift)
                            seeds = {1 + 2 * slot + half: (vector >> (64 * half)) & MASK64
                                     for slot, vector in enumerate(vectors) for half in range(2)}
                            seeds.update({27: 0x9F | (QC if sticky else 0), 28: 0x01400000})
                            initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                               (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                                0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                            word = base | ((1 if bits == 16 else 2) << 22)
                            word |= (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                            if form == "idx":
                                word |= ((index >> (2 if bits == 16 else 1)) & 1) << 11
                                word |= ((index >> (1 if bits == 16 else 0)) & 1) << 21
                                if bits == 16:
                                    word |= (index & 1) << 20
                            capture = (0xD53B443B, 0xD53B441C) + tuple(
                                0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                            name = f"scalarmulhigh_{form}_{op}_e{bits}_i{index}_a{mode}_{profile}_qc{sticky}"
                            words = (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture
                            cases.append(case_type(name, words, name, seeds, 0xB0000000,
                                                   "scalarmulhigh", bytes(range(64))))
    if len(cases) != 1960 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"Scalar high multiply coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, form, op, width, index_text, alias_text, _, _ = case.name.split("_")
    bits, index, mode = int(width[1:]), int(index_text[1:]), int(alias_text[1:])
    rd, rn, rm = ALIASES[mode]
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]
    mask, sign = (1 << bits) - 1, 1 << (bits - 1)

    def signed(raw):
        value = raw & mask
        return value - (1 << bits) if value & sign else value

    product = 2 * signed(vectors[rn]) * signed(vectors[rm] >> (bits * index if form == "idx" else 0))
    if op == "sqrdmulh":
        product += sign
    exact = product // (1 << bits)
    result = max(-sign, min(sign - 1, exact))
    vectors[rd] = result & mask
    fpsr = case.seeds[27] | (QC if result != exact else 0)
    registers = {f"x{n}": case.seeds.get(n, 0x100 + n) for n in range(29)}
    registers.update({"x27": fpsr, "x28": case.seeds[28],
                      "x29": differential.MEMORY_BASE, "x30": differential.DATA_BASE})
    return {"memory": b"".join(value.to_bytes(16, "little") for value in vectors),
            "nzcv": case.nzcv, "registers": registers}


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
        op = case.name.split("_")[2]
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
            raise ValueError(f"{case.name}: independent exact integer/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} scalar high multiply/exact rounding/QC/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
