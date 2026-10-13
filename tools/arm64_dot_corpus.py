"""SDOT/UDOT with byte-wise Python arithmetic and complete ELF snapshots."""

import argparse
import random
import struct


ALIASES = ((0, 1, 2), (1, 1, 2), (2, 1, 2), (0, 2, 2), (2, 2, 2))
MASK64 = (1 << 64) - 1


def physical_registers(rm):
    rn = 31 if rm != 31 else 30
    rd = 0 if rm != 0 else 1
    sentinel = next(reg for reg in (3, 2, 4, 5) if reg not in (rd, rn, rm))
    return rd, rn, rm, sentinel


def corpus(case_type):
    rng, cases = random.Random(0xD0742026), []
    edges = (0, 1, 127, 128, 129, 254, 255, 17)

    def add(form, op, q, index, mode, rm, profile):
        physical = physical_registers(rm)
        rd, rn, rhs = ALIASES[mode]
        vectors = [rng.getrandbits(128).to_bytes(16, "little") for _ in range(4)]
        if profile < 8:
            left = bytes(edges[(profile + n) % 8] for n in range(16))
            right = bytes(edges[(profile + 3 * n) % 8] for n in range(16))
            if profile == 0:
                left, right = bytes([128]) * 16, bytes([128]) * 16
            elif profile == 1:
                left, right = bytes([255]) * 16, bytes([255]) * 16
            elif profile == 2:
                left, right = bytes([128]) * 16, bytes([127]) * 16
            elif profile == 3:
                left, right = bytes([1]) * 16, bytes([1]) * 16
            elif profile == 4:
                left, right = bytes([255]) * 16, bytes([1]) * 16
            vectors[rn], vectors[rhs] = left, right
            if rd not in (rn, rhs):
                # Positive and negative products cross modulo-32 boundaries;
                # Q=0 must discard the poisoned upper accumulator lanes.
                vectors[rd] = struct.pack("<4I", 0xFFFFFFFF, 0, 0x7FFFFFFF, 0x80000000)
        seeds = {1 + slot * 2 + half: int.from_bytes(vector[half * 8:half * 8 + 8], "little")
                 for slot, vector in enumerate(vectors) for half in range(2)}
        seeds.update({27: 0x0800009F if (profile + index + mode) & 1 else 0x91,
                      28: 0x01400000})
        initialize = tuple(word for slot, reg in enumerate(physical) for word in
                           (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                            0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
        base = ({"sdot": 0x0E009400, "udot": 0x2E009400} if form == "reg" else
                {"sdot": 0x0F00E000, "udot": 0x2F00E000})[op]
        word = base | (2 << 22) | (q << 30) | (physical[rhs] << 16) | (physical[rn] << 5) | physical[rd]
        if form == "idx":
            word |= ((index >> 1) << 11) | ((index & 1) << 21)
        capture = (0xD53B443B, 0xD53B441C) + tuple(
            0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
        name = f"dot_{form}_{op}_q{q}_i{index}_a{mode}_m{rm}_p{profile}"
        cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                               name, seeds, 0xB0000000, "dot", bytes(range(64))))

    for form in ("reg", "idx"):
        for op in ("sdot", "udot"):
            for q in (0, 1):
                for index in range(4 if form == "idx" else 1):
                    for mode in range(5):
                        for profile in range(9):
                            add(form, op, q, index, mode, 30, profile)
    # Sweep all 32 Rm encodings with every indexed group: bit M must select
    # registers 16..31, rather than becoming an extra byte-group index bit.
    for rm in range(32):
        for op in ("sdot", "udot"):
            for q in (0, 1):
                for index in range(4):
                    add("idx", op, q, index, 0, rm, 9)
    if len(cases) != 1412 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"Dot product coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, form, op, q_text, index_text, alias_text, _, _ = case.name.split("_")
    q, index, mode = int(q_text[1:]), int(index_text[1:]), int(alias_text[1:])
    rd, rn, rm = ALIASES[mode]
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    left, right, old = vectors[rn], vectors[rm], vectors[rd]
    result = bytearray(16)
    for lane in range(4 if q else 2):
        accumulator = int.from_bytes(old[lane * 4:lane * 4 + 4], "little")
        for byte in range(4):
            a, b = left[lane * 4 + byte], right[(index if form == "idx" else lane) * 4 + byte]
            if op == "sdot":
                a = a - 256 if a >= 128 else a
                b = b - 256 if b >= 128 else b
            accumulator += a * b
        result[lane * 4:lane * 4 + 4] = (accumulator & 0xFFFFFFFF).to_bytes(4, "little")
    vectors[rd] = bytes(result)
    registers = {f"x{n}": case.seeds.get(n, 0x100 + n) for n in range(29)}
    registers.update({"x29": differential.MEMORY_BASE, "x30": differential.DATA_BASE})
    return {"memory": b"".join(vectors), "registers": registers, "nzcv": case.nzcv,
            "fpsr": case.seeds[27], "fpcr": case.seeds[28]}


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
        if len(decoded) * 4 != len(code) or sum(instruction.mnemonic == case.name.split("_")[2] for instruction in decoded) != 1:
            raise ValueError(f"{case.name}: Capstone did not decode the intended operation/harness")
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
            raise ValueError(f"{case.name}: independent byte sum/modulo/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} SDOT/UDOT byte arithmetic/wrap/aliases/full Rm/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
