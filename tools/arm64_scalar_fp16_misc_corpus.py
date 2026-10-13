"""Scalar FP16 zero comparisons and FRECPX: independent bit oracle, full ELF snapshots."""

import argparse
import struct

OPS = ("fcmeq", "fcmgt", "fcmge", "fcmlt", "fcmle", "frecpx")
BASES = (0x5EF8D800,0x5EF8C800,0x7EF8C800,0x5EF8E800,0x7EF8D800,0x5EF9F800)
CONTROLS = (0,1 << 22,2 << 22,3 << 22,1 << 24,1 << 25,(1 << 19) | (1 << 25),1 << 19)
ALIASES = ((0,1),(1,1),(2,1))
VALUES = (0,0x8000,1,0x8001,0x0400,0x8400,0x3C00,0xBC00,
          0x7BFF,0xFBFF,0x7C00,0xFC00,0x7E03,0xFE05,0x7C01,0xFC03)


def reference(value, op, fpcr):
    exponent, fraction, sign = (value >> 10) & 31, value & 1023, value & 0x8000
    if exponent == 31 and fraction:
        signaling = not fraction & 512
        if op == "frecpx":
            return 0x7E00 if fpcr & (1 << 25) else value | 512, 1 if signaling else 0
        return 0, 1 if signaling or op != "fcmeq" else 0
    if not exponent and fraction and fpcr & (1 << 19):
        value, fraction = sign, 0
    if op == "frecpx":
        return sign | ((30 if not exponent else 31 - exponent) << 10), 0
    zero = not value & 0x7FFF
    negative = bool(sign)
    match = {"fcmeq": zero,"fcmgt": not zero and not negative,"fcmge": zero or not negative,
             "fcmlt": not zero and negative,"fcmle": zero or negative}[op]
    return 0xFFFF if match else 0, 0


def corpus(case_type):
    cases, physical = [], (0,31,30,3)
    for operation, op in enumerate(OPS):
        for alias,(rd,rn) in enumerate(ALIASES):
            for control,fpcr in enumerate(CONTROLS):
                for profile,value in enumerate(VALUES):
                    vectors = [0x112233445566778899AABBCCDDEEFF00 ^ slot for slot in range(4)]
                    vectors[rn] = (0xFC017C0380010001 << 64) | (0xFE017C014000 << 16) | value
                    seeds = {1 + slot*2 + part:(vector >> (part*64)) & ((1 << 64)-1)
                             for slot,vector in enumerate(vectors) for part in range(2)}
                    seeds.update({27:0x08000002,28:fpcr})
                    initialize = tuple(word for slot,reg in enumerate(physical) for word in
                                       (0x9E670000 | ((slot*2+1) << 5) | reg,0x9EAF0000 | ((slot*2+2) << 5) | reg))
                    word = BASES[operation] | (physical[rn] << 5) | physical[rd]
                    capture = (0xD53B443B,0xD53B441C) + tuple(0x3D8003A0 | (slot << 10) | reg for slot,reg in enumerate(physical))
                    name = f"misc16_{op}_a{alias}_c{control}_p{profile}"
                    cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,
                                           name,seeds,0xB0000000,"misc16",bytes(range(64))))
    if len(cases) != 2304 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"Scalar FP16 misc coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _,op,alias_text,_,_ = case.name.split("_")
    rd,rn = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n+1] << 64) for n in (1,3,5,7)]
    value,flags = reference(vectors[rn] & 65535,op,case.seeds[28])
    vectors[rd],fpsr = value,case.seeds[27] | flags
    registers = {f"x{n}":case.seeds.get(n,0x100+n) for n in range(29)}
    registers.update({"x27":fpsr,"x28":case.seeds[28],"x29":differential.MEMORY_BASE,"x30":differential.DATA_BASE})
    return {"memory":b"".join(vector.to_bytes(16,"little") for vector in vectors),
            "registers":registers,"nzcv":case.nzcv,"fpsr":fpsr,"fpcr":case.seeds[28]}


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
            raise ValueError(f"{case.name}: independent scalar FP16 comparison/reciprocal exponent bit model differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} scalar FP16 compare-zero/FRECPX/bit oracle/FZ16/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
