"""Bounded RCpc unscaled-memory corpus with byte-array architectural oracle.

Tests normal memory, not multi-core acquire/release ordering.
"""

import argparse


BASES = {"stlurb": (0x19000000, 8, False, False, 32),
         "stlurh": (0x59000000, 16, False, False, 32),
         "stlurw": (0x99000000, 32, False, False, 32),
         "stlurx": (0xD9000000, 64, False, False, 64),
         "ldapurb": (0x19400000, 8, True, False, 32),
         "ldapurh": (0x59400000, 16, True, False, 32),
         "ldapurw": (0x99400000, 32, True, False, 32),
         "ldapurx": (0xD9400000, 64, True, False, 64),
         "ldapursbw": (0x19C00000, 8, True, True, 32),
         "ldapursbx": (0x19800000, 8, True, True, 64),
         "ldapurshw": (0x59C00000, 16, True, True, 32),
         "ldapurshx": (0x59800000, 16, True, True, 64),
         "ldapursw": (0x99800000, 32, True, True, 64)}
OFFSETS = (-256, -255, -129, -64, -1, 0, 1, 7, 31, 63, 254, 255)


def corpus(case_type):
    import arm64_differential as differential
    cases = []
    fixture_base = differential.MEMORY_BASE
    patterns = (bytes((0x80 + 17 * i) & 255 for i in range(64)),
                bytes([255]) * 64, bytes((i * 3 + 1) & 127 for i in range(64)))
    for op, (base, bits, load, signed, destination) in BASES.items():
        for offset in OFFSETS:
            choices = [(mode, profile) for mode in ("distinct", "sp", "zr") for profile in range(3)]
            choices += [("alias", 0)]
            for mode, profile in choices:
                target = 16 + (offset & 15) if mode == "sp" else (1, 15, 31)[profile]
                address = fixture_base + target - offset
                seeds = {0: 0xF1E2D3C4B5A69788, 1: address, 26: 0,
                         27: 0x0800009F, 28: 0x01400000}
                if mode == "alias":
                    seeds[0], seeds[1] = address, 0x101
                rn = 31 if mode == "sp" else 0 if mode == "alias" else 1
                rt = 31 if mode == "zr" else 0
                words = [0xD51B443B, 0xD51B441C]
                if mode == "sp":
                    words += [0x9100003F]  # ADD SP, X1, #0; base is 16-byte aligned.
                words += [base | ((offset & 511) << 12) | (rn << 5) | rt]
                if mode != "alias":
                    words += [0xCB1D0021]  # SUB X1,X1,X29 normalizes host-independent base.
                elif not load:
                    words += [0xCB1D0000]  # Store/base alias retains base, normalize it.
                if mode == "sp":
                    words += [0x910003FA, 0xCB1D035A]  # Read and normalize SP in X26.
                words += [0xD53B443B, 0xD53B441C]
                name = f"rcpc_{op}_o{offset}_p{profile}_{mode}"
                cases.append(case_type(name, tuple(words), name, seeds, 0xB0000000, "rcpc", patterns[profile]))
    if len(cases) != 1560:
        raise ValueError(f"RCpc coverage count changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, op, offset, profile, mode = case.name.split("_")
    offset, profile = int(offset[1:]), int(profile[1:])
    base, bits, load, signed, destination = BASES[op]
    target = 16 + (offset & 15) if mode == "sp" else (1, 15, 31)[profile]
    registers = {f"x{n}": case.seeds.get(n, 0x100 + n) for n in range(29)}
    memory = bytearray(case.memory)
    if load:
        value = int.from_bytes(memory[target:target + bits // 8], "little", signed=signed)
        if mode != "zr":
            registers["x0"] = value & ((1 << destination) - 1)
    else:
        value = 0 if mode == "zr" else case.seeds[0]
        memory[target:target + bits // 8] = (value & ((1 << bits) - 1)).to_bytes(bits // 8, "little")
    if mode != "alias":
        registers["x1"] = (target - offset) & differential.MASK64
    elif not load:
        registers["x0"] = (target - offset) & differential.MASK64
    if mode == "sp":
        registers["x26"] = (target - offset) & differential.MASK64
    return {"registers": registers, "memory": bytes(memory), "nzcv": case.nzcv}


def verify():
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case)
    for case in cases:
        code = b"".join(word.to_bytes(4, "little") for word in case.instructions)
        if len(list(decoder.disasm(code, 0x400000))) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected code")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(0x400000, 4096)
        cpu.mem_map(differential.MEMORY_BASE & ~4095, 4096)
        cpu.mem_write(0x400000, code)
        cpu.mem_write(differential.MEMORY_BASE, case.memory)
        for n in range(29):
            cpu.reg_write(getattr(registers, f"UC_ARM64_REG_X{n}"), case.seeds.get(n, 0x100 + n))
        cpu.reg_write(registers.UC_ARM64_REG_X29, differential.MEMORY_BASE)
        cpu.reg_write(registers.UC_ARM64_REG_NZCV, case.nzcv)
        cpu.emu_start(0x400000, 0x400000 + len(code))
        wanted = expected(case)
        for name, value in wanted["registers"].items():
            actual = cpu.reg_read(getattr(registers, f"UC_ARM64_REG_{name.upper()}"))
            if actual != value:
                raise ValueError(f"{case.name}: {name} reference {value:#x} != Unicorn {actual:#x}")
        if (bytes(cpu.mem_read(differential.MEMORY_BASE, 64)) != wanted["memory"] or
                cpu.reg_read(registers.UC_ARM64_REG_NZCV) != case.nzcv):
            raise ValueError(f"{case.name}: byte-array/NZCV reference differs from Unicorn")
    print(f"PASS {len(cases)} RCpc independent byte-array/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
