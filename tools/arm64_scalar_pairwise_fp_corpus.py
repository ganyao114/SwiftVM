"""H/S/D scalar FP pairwise: exact integer addition and min/max bit oracle."""

import argparse
import random
import struct


OPS = ("faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp")
ALIASES = ((0, 1), (1, 1), (2, 1))
CONTROLS = (0, 1 << 22, 2 << 22, 3 << 22, 1 << 24, 1 << 25,
            (1 << 24) | (1 << 25), 1 << 19)
MASK64 = (1 << 64) - 1


def reference(a, b, bits, op, fpcr):
    fraction = {16: 10, 32: 23, 64: 52}[bits]
    bias = {16: 15, 32: 127, 64: 1023}[bits]
    maximum_exponent, sign = 2 * bias + 1, 1 << (bits - 1)
    fraction_mask = (1 << fraction) - 1
    exponent_mask, quiet = maximum_exponent << fraction, 1 << (fraction - 1)
    flush, flags = bool(fpcr & (1 << (19 if bits == 16 else 24))), 0

    def nan(value):
        return value & exponent_mask == exponent_mask and bool(value & fraction_mask)

    def signaling(value):
        return nan(value) and not value & quiet

    if flush:
        if not a & exponent_mask and a & fraction_mask:
            a &= sign
            if bits != 16:
                flags |= 128
        if not b & exponent_mask and b & fraction_mask:
            b &= sign
            if bits != 16:
                flags |= 128
    numeric, minimum = op in ("fmaxnmp", "fminnmp"), op in ("fminp", "fminnmp")
    snan = signaling(a) or signaling(b)
    if snan or ((nan(a) or nan(b)) and (op == "faddp" or not numeric or (nan(a) and nan(b)))):
        if snan:
            flags |= 1
        selected = a if signaling(a) else b if signaling(b) else a if nan(a) else b
        return (exponent_mask | quiet if fpcr & (1 << 25) else selected | quiet), flags
    if nan(a):
        return b, flags
    if nan(b):
        return a, flags
    if op != "faddp":
        if not a & ~sign and not b & ~sign:
            return a | b if minimum else a & b, flags
        less = bool(a & sign) if bool(a & sign) != bool(b & sign) else (a > b if a & sign else a < b)
        return a if minimum == less else b, flags
    ainf, binf = a & ~sign == exponent_mask, b & ~sign == exponent_mask
    if ainf or binf:
        if ainf and binf and bool(a & sign) != bool(b & sign):
            return exponent_mask | quiet, flags | 1
        return a if ainf else b, flags

    def magnitude(value):
        exponent, significand = (value & exponent_mask) >> fraction, value & fraction_mask
        return ((significand | (1 << fraction)) << (exponent - 1)) if exponent else significand

    exact = (-magnitude(a) if a & sign else magnitude(a)) + (-magnitude(b) if b & sign else magnitude(b))
    rounding = (fpcr >> 22) & 3
    if not exact:
        zero_sign = a & sign if bool(a & sign) == bool(b & sign) else sign if rounding == 2 else 0
        return zero_sign, flags
    negative, exact = exact < 0, abs(exact)
    result_sign = sign if negative else 0
    if flush and exact < 1 << fraction:
        return result_sign, flags | 8
    shift = max(0, exact.bit_length() - 1 - fraction)
    rounded, remainder = exact >> shift, exact - ((exact >> shift) << shift)
    if remainder:
        flags |= 16
        increment = not negative if rounding == 1 else negative if rounding == 2 else False
        if rounding == 0:
            half = 1 << (shift - 1)
            increment = remainder > half or (remainder == half and bool(rounded & 1))
        rounded += increment
    if rounded >= 1 << (fraction + 1):
        rounded >>= 1
        shift += 1
    exponent = 0 if rounded < 1 << fraction else shift + 1
    if exponent >= maximum_exponent:
        infinity = rounding == 0 or (rounding == 1 and not negative) or (rounding == 2 and negative)
        return result_sign | (exponent_mask if infinity else exponent_mask - 1), flags | 20
    return result_sign | (exponent << fraction) | (rounded & fraction_mask), flags


def corpus(case_type):
    rng, cases = random.Random(0xFA1F0152026), []
    physical = (0, 31, 30, 3)
    for bits in (16, 32, 64):
        fraction, bias = {16: 10, 32: 23, 64: 52}[bits], {16: 15, 32: 127, 64: 1023}[bits]
        sign, mask = 1 << (bits - 1), (1 << bits) - 1
        infinity, one, quiet = (2 * bias + 1) << fraction, bias << fraction, 1 << (fraction - 1)
        for operation, op in enumerate(OPS):
            base = (0x5E30D800, 0x5E30F800, 0x5EB0F800, 0x5E30C800, 0x5EB0C800)[operation]
            if bits != 16:
                base |= (1 << 29) | ((1 << 22) if bits == 64 else 0)
            for alias, (rd, rn) in enumerate(ALIASES):
                for control, fpcr in enumerate(CONTROLS):
                    for profile in range(8):
                        vectors = [rng.getrandbits(128) for _ in range(4)]
                        if profile == 0:
                            lhs, rhs = rng.getrandbits(bits), rng.getrandbits(bits)
                        elif profile == 1:
                            lhs, rhs = one + (alias & 1), (bias - fraction - 1) << fraction
                        elif profile == 2:
                            lhs, rhs = sign, 0
                        elif profile == 3:
                            lhs, rhs = infinity, infinity | sign
                        elif profile == 4:
                            lhs, rhs = infinity | quiet | 3, (infinity | quiet | 7) if alias == 2 else one
                            if alias == 1:
                                lhs, rhs = rhs, lhs
                        elif profile == 5:
                            lhs, rhs = infinity | quiet | 9, infinity | sign | 5
                            if alias == 1:
                                lhs, rhs = rhs, lhs
                            if alias == 2:
                                lhs &= ~quiet
                        elif profile == 6:
                            lhs, rhs = (1 << fraction) + 1, (1 << fraction) | sign
                            if alias == 1:
                                lhs, rhs = 1, sign | 1
                            if alias == 2:
                                lhs, rhs = (1 << fraction) - 1, one
                        else:
                            lhs = rhs = infinity - 1
                            if alias == 1:
                                lhs |= sign
                                rhs |= sign
                        low_mask = (1 << (bits * 2)) - 1
                        vectors[rn] = (vectors[rn] & ~low_mask) | (lhs & mask) | ((rhs & mask) << bits)
                        seeds = {1 + slot * 2 + part: (vector >> (part * 64)) & MASK64
                                 for slot, vector in enumerate(vectors) for part in range(2)}
                        seeds.update({27: 0x08000002, 28: fpcr})
                        initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                           (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                            0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                        instruction = base | (physical[rn] << 5) | physical[rd]
                        capture = (0xD53B443B, 0xD53B441C) + tuple(
                            0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                        name = f"pairfp_{op}_e{bits}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (instruction,) + capture,
                                               name, seeds, 0xB0000000, "pairfp", bytes(range(64))))
    if len(cases) != 2880 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"Scalar FP pairwise coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, op, width, alias_text, _, _ = case.name.split("_")
    bits, alias = int(width[1:]), int(alias_text[1:])
    rd, rn = ALIASES[alias]
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]
    mask = (1 << bits) - 1
    result, flags = reference(vectors[rn] & mask, (vectors[rn] >> bits) & mask, bits, op, case.seeds[28])
    vectors[rd] = result
    fpsr = case.seeds[27] | flags
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
            raise ValueError(f"{case.name}: independent FP addition/min/max bit model/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} scalar FP pairwise/exact integer addition/NaN/FZ/DN/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
