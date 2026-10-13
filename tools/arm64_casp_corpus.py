"""CASP X-pair differential fixtures with an independent byte-array oracle.

Checks single-thread pair compare/store semantics; concurrency is covered by
the backend's separate shared-memory tests.
"""

import argparse


MODES = ((0, 2, 4), (0, 0, 4), (0, 2, 0), (0, 2, 1), (0, 2, 2),
         (0, 2, 3), (30, 2, 4), (0, 30, 4), (30, 30, 31), (0, 2, 31))
MASK64 = (1 << 64) - 1


def corpus(case_type):
    import arm64_differential as differential
    cases = []
    address = differential.MEMORY_BASE + 16
    for order in range(4):
        for mode, (rs, rt, rn) in enumerate(MODES):
            for mismatch in range(4):
                seeds = {i: (0x1020304050607080 + i * 0x0101010101010101) & MASK64 for i in range(25)}
                seeds.update({25: 0xFEEDFACEDEADBEEF, 26: 0, 27: 0x08000091, 28: 0x01400000})
                words = [0xD51B443B, 0xD51B441C, 0xAA1903FE]  # Controls, MOV X30,X25.
                if rn == 31:
                    seeds[23] = address
                    words.append(0x910002FF)  # ADD SP,X23,#0.
                else:
                    seeds[rn] = address
                values = dict(seeds)
                values[30], values[31] = seeds[25], 0
                pair = (values[rs] ^ (1 if mismatch & 1 else 0),
                        values[rs + 1] ^ ((1 << 63) if mismatch & 2 else 0))
                memory = bytearray((0x93 + i * 13) & 255 for i in range(64))
                memory[16:32] = b"".join(value.to_bytes(8, "little") for value in pair)
                word = (0x48207C00 | (0x00400000 if order & 1 else 0)
                        | (0x8000 if order & 2 else 0) | (rs << 16) | (rn << 5) | rt)
                words.append(word)
                if rn != 31 and rn not in (rs, rs + 1):
                    words.append(0xCB000000 | (29 << 16) | (rn << 5) | rn)  # Normalize base.
                if rn == 31:
                    words += [0xCB1D02F7, 0x910003FA, 0xCB1D035A]  # Normalize X23 and SP in X26.
                words += [0xAA1E03F8, 0xD53B443B, 0xD53B441C]  # Export X30 in X24.
                words += differential.mov64(30, differential.DATA_BASE)  # Restore snapshot pointer.
                name = f"casp_o{order}_m{mode}_d{mismatch}"
                cases.append(case_type(name, tuple(words), name, seeds, 0xB0000000, "casp", bytes(memory)))
    return cases


def expected(case):
    import arm64_differential as differential
    _, order_text, mode_text, mismatch_text = case.name.split("_")
    mode, mismatch = int(mode_text[1:]), int(mismatch_text[1:])
    rs, rt, rn = MODES[mode]
    values = {i: case.seeds.get(i, 0x100 + i) for i in range(29)}
    values[30], values[31] = values[25], 0
    memory = bytearray(case.memory)
    expected_pair = values[rs].to_bytes(8, "little") + values[rs + 1].to_bytes(8, "little")
    desired_pair = values[rt].to_bytes(8, "little") + values[rt + 1].to_bytes(8, "little")
    observed_pair = bytes(memory[16:32])
    if observed_pair == expected_pair:
        memory[16:32] = desired_pair
    values[rs] = int.from_bytes(observed_pair[:8], "little")
    if rs + 1 != 31:
        values[rs + 1] = int.from_bytes(observed_pair[8:], "little")
    if rn != 31 and rn not in (rs, rs + 1):
        values[rn] = (values[rn] - differential.MEMORY_BASE) & MASK64
    if rn == 31:
        values[23], values[26] = 16, 16
    values[24] = values[30]
    return {"registers": {f"x{i}": values[i] for i in range(29)},
            "memory": bytes(memory), "nzcv": case.nzcv}


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
        ids = [getattr(registers, f"UC_ARM64_REG_X{i}") for i in range(29)]
        for i, reg in enumerate(ids):
            cpu.reg_write(reg, case.seeds.get(i, 0x100 + i))
        cpu.reg_write(registers.UC_ARM64_REG_X29, differential.MEMORY_BASE)
        cpu.reg_write(registers.UC_ARM64_REG_NZCV, case.nzcv)
        cpu.emu_start(0x400000, 0x400000 + len(code))
        wanted = expected(case)
        for i, reg in enumerate(ids):
            if cpu.reg_read(reg) != wanted["registers"][f"x{i}"]:
                raise ValueError(f"{case.name}: x{i} byte-array/Unicorn disagreement")
        if bytes(cpu.mem_read(differential.MEMORY_BASE, 64)) != wanted["memory"]:
            raise ValueError(f"{case.name}: memory byte-array/Unicorn disagreement")
        if cpu.reg_read(registers.UC_ARM64_REG_NZCV) != case.nzcv:
            raise ValueError(f"{case.name}: NZCV changed")
        if cpu.reg_read(registers.UC_ARM64_REG_X30) != differential.DATA_BASE:
            raise ValueError(f"{case.name}: snapshot pointer was not restored")
        if cpu.reg_read(registers.UC_ARM64_REG_X29) != differential.MEMORY_BASE:
            raise ValueError(f"{case.name}: fixture memory pointer changed")
    print(f"PASS {len(cases)} CASP byte-array/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
