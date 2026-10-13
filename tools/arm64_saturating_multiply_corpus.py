"""SQDMULH/SQRDMULH/SQRDMLAH/SQRDMLSH: exact final saturation ELF oracle."""

import argparse
import random
import struct


ALIASES = ((0, 1, 2), (1, 1, 2), (2, 1, 2), (0, 2, 2), (2, 2, 2))
OPS = ("sqdmulh", "sqrdmulh", "sqrdmlah", "sqrdmlsh")
QC = 1 << 27
MASK64 = (1 << 64) - 1


def corpus(case_type):
    rng, cases = random.Random(0x5A7D01712026), []
    for bits in (16, 32):
        mask, sign, half = (1 << bits) - 1, 1 << (bits - 1), 1 << (bits - 2)
        physical = (0, 31, 15 if bits == 16 else 30, 3)

        def pack(lanes):
            return sum((value & mask) << (lane * bits) for lane, value in enumerate(lanes))

        for op_index, op in enumerate(OPS):
            for shape in ("s", "q0", "q1"):
                active = 1 if shape == "s" else (64 if shape == "q0" else 128) // bits
                for form in ("reg", "idx"):
                    for index in range(128 // bits if form == "idx" else 1):
                        for alias, (rd, rn, rm) in enumerate(ALIASES):
                            profiles = ("random", "cancel", "ties")
                            if alias == 0:
                                profiles += ("overflow", "inactive")
                            for profile_index, profile in enumerate(profiles):
                                vectors = [rng.getrandbits(128) for _ in range(4)]
                                if profile != "random":
                                    if profile in ("cancel", "overflow"):
                                        lhs = rhs = [-sign] * (128 // bits)
                                        accumulator = [-1 if op_index == 2 else 1 if op_index == 3 else 0] * (128 // bits)
                                        if profile == "overflow":
                                            accumulator = [-sign if op_index == 3 else 0] * (128 // bits)
                                    elif profile == "ties":
                                        lhs = [-1 if lane % 2 == 0 else 1 for lane in range(128 // bits)]
                                        rhs = [half + (lane % 3 - 1) for lane in range(128 // bits)]
                                        # Scalar/indexed profiles alternate both signs and exact ties.
                                        lhs[0] = -1 if index % 2 == 0 else 1
                                        rhs[index if form == "idx" else 0] = half
                                        accumulator = [0] * (128 // bits)
                                    else:
                                        lhs = [0 if lane < active else -sign for lane in range(128 // bits)]
                                        rhs = [-sign] * (128 // bits)
                                        accumulator = [0 if lane < active else (-sign if op_index == 3 else sign - 1)
                                                       for lane in range(128 // bits)]
                                    vectors[rd] = pack(accumulator)
                                    vectors[rn] = pack(lhs)
                                    vectors[rm] = pack(rhs)
                                sticky = (profile_index + alias + index) & 1
                                # Inactive-overflow cases must start with QC clear.
                                if profile == "inactive":
                                    sticky = 0
                                seeds = {1 + slot * 2 + part: (vector >> (64 * part)) & MASK64
                                         for slot, vector in enumerate(vectors) for part in range(2)}
                                seeds.update({27: 0x91 | (QC if sticky else 0), 28: 0x01400000})
                                initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                                   (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                                    0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                                base = ((0x0E20B400, 0x2E20B400, 0x2E008400, 0x2E008C00) if form == "reg" else
                                        (0x0F00C000, 0x0F00D000, 0x2F00D000, 0x2F00F000))[op_index]
                                word = base | ((1 if bits == 16 else 2) << 22)
                                word |= 0x50000000 if shape == "s" else 0x40000000 if shape == "q1" else 0
                                word |= (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                                if form == "idx":
                                    word |= ((index >> (2 if bits == 16 else 1)) & 1) << 11
                                    word |= ((index >> (1 if bits == 16 else 0)) & 1) << 21
                                    if bits == 16:
                                        word |= (index & 1) << 20
                                capture = (0xD53B443B, 0xD53B441C) + tuple(
                                    0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                                name = f"satmul_{form}_{op}_{shape}_e{bits}_i{index}_a{alias}_{profile}_qc{sticky}"
                                words = (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture
                                cases.append(case_type(name, words, name, seeds, 0xB0000000,
                                                       "satmul", bytes(range(64))))
    if len(cases) != 2856 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"Saturating multiply coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, form, op, shape, width, index_text, alias_text, _, _ = case.name.split("_")
    bits, index, alias = int(width[1:]), int(index_text[1:]), int(alias_text[1:])
    mode, (rd, rn, rm) = OPS.index(op), ALIASES[alias]
    mask, sign, denominator = (1 << bits) - 1, 1 << (bits - 1), 1 << bits
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]

    def signed(vector, lane):
        value = (vector >> (lane * bits)) & mask
        return value - denominator if value & sign else value

    left, right, old = vectors[rn], vectors[rm], vectors[rd]
    result, fpsr = 0, case.seeds[27]
    active = 1 if shape == "s" else (64 if shape == "q0" else 128) // bits
    for lane in range(active):
        numerator = 2 * signed(left, lane) * signed(right, index if form == "idx" else lane)
        if mode == 2:
            numerator += signed(old, lane) * denominator
        elif mode == 3:
            numerator = signed(old, lane) * denominator - numerator
        if mode:
            numerator += sign
        exact = numerator // denominator
        saturated = max(-sign, min(sign - 1, exact))
        if saturated != exact:
            fpsr |= QC
        result |= (saturated & mask) << (lane * bits)
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
            raise ValueError(f"{case.name}: independent final saturation integer/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} saturating multiply/final saturation/RDM ties/QC/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
