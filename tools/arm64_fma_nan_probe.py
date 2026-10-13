"""Full ELF Unicorn probe for ARM FMA NaN source/sign/invalid-product priority."""

import argparse
import itertools
import struct

OPS = ("fmadd", "fmsub", "fnmadd", "fnmsub")
MASK64 = (1 << 64) - 1


def oracle(a, b, c, bits, op, fpcr):
    fraction = {16: 10, 32: 23, 64: 52}[bits]
    bias = {16: 15, 32: 127, 64: 1023}[bits]
    sign, exponent, quiet = 1 << (bits - 1), (2 * bias + 1) << fraction, 1 << (fraction - 1)
    fraction_mask = (1 << fraction) - 1
    if op >= 2:
        c ^= sign
    if bool(op & 1) != bool(op >= 2):
        a ^= sign

    def nan(value):
        return value & exponent == exponent and bool(value & fraction_mask)

    def signaling(value):
        return nan(value) and not value & quiet

    # ARM FPMulAdd processes addend, first multiplicand, second multiplicand.
    # An infinity*zero invalid product overrides quiet NaNs, but signaling
    # NaNs retain the operand-selection rule and raise invalid themselves.
    ordered = (c, a, b)
    invalid = ((a & ~sign == exponent and not b & ~sign) or
               (b & ~sign == exponent and not a & ~sign))
    selected = next((value for value in ordered if signaling(value)), None)
    flags = 1 if selected is not None or invalid else 0
    if invalid and selected is None:
        return exponent | quiet, flags
    if selected is None:
        selected = next((value for value in ordered if nan(value)), None)
    if selected is None:
        return None
    return exponent | quiet if fpcr & (1 << 25) else selected | quiet, flags


def corpus(case_type):
    cases, physical = [], (0, 31, 30, 3)
    for bits in (16, 32, 64):
        fraction = {16: 10, 32: 23, 64: 52}[bits]
        bias = {16: 15, 32: 127, 64: 1023}[bits]
        sign, exponent, quiet = 1 << (bits - 1), (2 * bias + 1) << fraction, 1 << (fraction - 1)
        left = (bias << fraction, exponent | quiet | 1, sign | exponent | 2, exponent, 0)
        right = ((bias + 1) << fraction, sign | exponent | quiet | 3, exponent | 4, 0, exponent)
        addend = (exponent | quiet | 5, sign | exponent | 6, bias << fraction)
        for op, name in enumerate(OPS):
            for dn in (0, 1):
                for profile, (a, b, c) in enumerate(itertools.product(left, right, addend)):
                    if oracle(a, b, c, bits, op, dn << 25) is None:
                        continue
                    vectors = (0xDEAD0123456789ABCDEF0FEDCBA987654,
                               a | (0x1122334455667788 << 64), b | (0x9988776655443322 << 64),
                               c | (0xCAFE123456789ABC << 64))
                    seeds = {1 + slot * 2 + part: (value >> (part * 64)) & MASK64
                             for slot, value in enumerate(vectors) for part in range(2)}
                    seeds.update({27: 0x08000002, 28: dn << 25})
                    initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                       (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                        0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                    word = 0x1F000000 | ({16: 3, 32: 0, 64: 1}[bits] << 22)
                    word |= ((op >= 2) << 21) | ((op & 1) << 15)
                    word |= (physical[2] << 16) | (physical[3] << 10) | (physical[1] << 5) | physical[0]
                    capture = (0xD53B443B, 0xD53B441C) + tuple(
                        0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                    case_name = f"fmanan_{name}_e{bits}_dn{dn}_p{profile}"
                    cases.append(case_type(case_name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                                           case_name, seeds, 0xB0000000, "fmanan", bytes(range(64))))
    return cases


def expected(case):
    import arm64_differential as differential
    _, op, width, dn_text, _ = case.name.split("_")
    bits, dn = int(width[1:]), int(dn_text[2:])
    mask = (1 << bits) - 1
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]
    value, flags = oracle(vectors[1] & mask, vectors[2] & mask, vectors[3] & mask, bits, OPS.index(op), dn << 25)
    vectors[0] = value
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
            raise ValueError(f"{case.name}: independent FMA NaN source/sign/invalid-product priority differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} H/S/D FMA NaN source/sign/invalid-product/DN/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
