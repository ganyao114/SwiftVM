"""FP16 vector binary and scalar FABD: exact rational arithmetic, full ELF oracle."""

import argparse
import random
import struct

from arm64_scalar_pairwise_fp_corpus import reference as pairwise_reference

OPS = ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fabd")
BASES = (0x0E401400, 0x0EC01400, 0x2E401C00, 0x2E403C00, 0x0E403400,
         0x0EC03400, 0x0E400400, 0x0EC00400, 0x2EC01400)
ALIASES = ((0, 1, 2), (1, 1, 2), (2, 1, 2), (0, 2, 2), (2, 2, 2))
CONTROLS = (0, 1 << 22, 2 << 22, 3 << 22, 1 << 24, 1 << 25,
            (1 << 19) | (1 << 25), 1 << 19)
MASK64 = (1 << 64) - 1


def reference(a, b, op, fpcr):
    if op in ("fmax", "fmin", "fmaxnm", "fminnm"):
        return pairwise_reference(a, b, 16, op.replace("nm", "nmp") if "nm" in op else op + "p", fpcr)
    absolute = op == "fabd"
    if absolute:
        op = "fsub"

    def finish(value, flags):
        return value & 0x7FFF if absolute else value, flags

    def nan(value):
        return value & 0x7C00 == 0x7C00 and bool(value & 1023)

    def signaling(value):
        return nan(value) and not value & 512

    if fpcr & (1 << 19):
        if not a & 0x7C00 and a & 1023:
            a &= 0x8000
        if not b & 0x7C00 and b & 1023:
            b &= 0x8000
    if nan(a) or nan(b):
        snan = signaling(a) or signaling(b)
        chosen = a if signaling(a) else b if signaling(b) else a if nan(a) else b
        return finish(0x7E00 if fpcr & (1 << 25) else chosen | 512, 1 if snan else 0)
    ainf, binf = a & 0x7FFF == 0x7C00, b & 0x7FFF == 0x7C00
    azero, bzero = not a & 0x7FFF, not b & 0x7FFF
    asign, bsign = bool(a & 0x8000), bool(b & 0x8000)
    mode = (fpcr >> 22) & 3

    def magnitude(value):
        exponent, fraction = (value >> 10) & 31, value & 1023
        return ((fraction | 1024) << (exponent - 1)) if exponent else fraction

    if op in ("fadd", "fsub"):
        if op == "fsub":
            bsign = not bsign
        if ainf or binf:
            if ainf and binf and asign != bsign:
                return finish(0x7E00, 1)
            return finish((0x8000 if (asign if ainf else bsign) else 0) | 0x7C00, 0)
        numerator = (-magnitude(a) if asign else magnitude(a)) + (-magnitude(b) if bsign else magnitude(b))
        denominator = 1 << 24
        if not numerator:
            sign = asign if asign == bsign else mode == 2
            return finish(0x8000 if sign else 0, 0)
        negative = numerator < 0
        numerator = abs(numerator)
    else:
        negative = asign != bsign
        if op == "fmul":
            if (ainf and bzero) or (binf and azero):
                return finish(0x7E00, 1)
            if ainf or binf:
                return finish((0x8000 if negative else 0) | 0x7C00, 0)
            numerator, denominator = magnitude(a) * magnitude(b), 1 << 48
        else:
            if (azero and bzero) or (ainf and binf):
                return finish(0x7E00, 1)
            if ainf or bzero:
                return finish((0x8000 if negative else 0) | 0x7C00, 2 if bzero and not ainf else 0)
            if binf:
                return finish(0x8000 if negative else 0, 0)
            numerator, denominator = magnitude(a), magnitude(b)
    sign = 0x8000 if negative else 0
    if not numerator:
        return finish(sign, 0)
    numerator <<= 24  # Rational in minimum-subnormal units.
    tiny = numerator < denominator * 1024
    if tiny and fpcr & (1 << 19):
        return finish(sign, 8)
    top = numerator.bit_length() - denominator.bit_length()
    if (numerator < denominator << top) if top >= 0 else (numerator << -top < denominator):
        top -= 1
    shift = max(0, top - 10)
    rounded, remainder = divmod(numerator, denominator << shift)
    flags = 0
    if remainder:
        flags |= 16 | (8 if tiny else 0)
        increment = (mode == 1 and not negative) or (mode == 2 and negative)
        if mode == 0:
            midpoint = denominator << shift
            increment = 2 * remainder > midpoint or (2 * remainder == midpoint and bool(rounded & 1))
        rounded += increment
    if rounded >= 2048:
        rounded >>= 1
        shift += 1
    exponent = 0 if rounded < 1024 else shift + 1
    if exponent >= 31:
        infinity = mode == 0 or (mode == 1 and not negative) or (mode == 2 and negative)
        return finish(sign | (0x7C00 if infinity else 0x7BFF), flags | 20)
    return finish(sign | (exponent << 10) | (rounded & 1023), flags)


def corpus(case_type):
    rng, cases, physical = random.Random(0xF016B1A42026), [], (0, 31, 30, 3)
    profiles = (
        ((0x7E03,0x7C05,0xFE07,0xFC09,0x3C00,0x7FFF,0x7C01,0xFE01),
         (0x7C01,0x7E09,0xFC03,0xFE03,0x7E01,0x3C00,0xFC05,0x7C01)),
        ((0,0x8000,0x7C00,0xFC00,0x7C00,0x3C00,0,0x8000),
         (0x8000,0,0xFC00,0x7C00,0,0,0x7C00,0xFC00)),
        ((0x3C00,0xBC00,0x3C01,0xBC01,0x3555,0xB555,0x3BFF,0xBBFF),
         (0x1000,0x1000,0x1000,0x1000,0x4200,0x4200,0x3C01,0xBC01)),
        ((1,0x8001,0x0400,0x8400,0x03FF,0x83FF,0x0401,0x8401),
         (0x3C00,0x3C00,0x4200,0x4200,1,0x8001,0x8400,0x0400)),
        ((0x7BFF,0xFBFF,0x7BFE,0xFBFE,0x7801,0xF801,0x7BFF,0xFBFF),
         (0x4000,0x4000,0x7BFF,0xFBFF,0x0801,0x0801,0x7BFF,0x7BFF)),
        ((0,0,0,0,0x7C01,0xBC00,0xFDFF,0xFC00),
         (0x3C00,0x3C00,0x3C00,0x3C00,0x7C03,0x7C00,0x7DFF,0x7C00)),
        ((0x3E00,0xBE00,0x4100,0xC100,0x3801,0xB801,0x5801,0xD801),
         (0x3800,0xB800,0x4400,0xC400,0x3801,0xB801,0x3555,0xB555)),
    )
    for op_index, op in enumerate(OPS):
        for shape in (("q0", "q1", "s") if op == "fabd" else ("q0", "q1")):
            for alias, (rd, rn, rm) in enumerate(ALIASES):
                for control, fpcr in enumerate(CONTROLS):
                    for profile in range(8):
                        vectors = [rng.getrandbits(128) for _ in range(4)]
                        left, right = profiles[profile - 1] if profile else (
                            tuple(rng.getrandbits(16) for _ in range(8)),
                            tuple(rng.getrandbits(16) for _ in range(8)))
                        # scalar source lane0 remains benign; every other lane
                        # carries an invalid-operation poison in this profile.
                        if profile == 6 and shape == "s":
                            left = (0,) + (0x7C01,) * 7
                            right = (0x3C00,) + (0xFC03,) * 7
                        vectors[rn] = sum(value << (lane * 16) for lane, value in enumerate(left))
                        vectors[rm] = sum(value << (lane * 16) for lane, value in enumerate(right))
                        seeds = {1 + slot * 2 + part: (vector >> (part * 64)) & MASK64
                                 for slot, vector in enumerate(vectors) for part in range(2)}
                        seeds.update({27: 0x08000002, 28: fpcr})
                        initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                           (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                            0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                        word = BASES[op_index] | (0x50000000 if shape == "s" else 0x40000000 if shape == "q1" else 0)
                        word |= (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                        capture = (0xD53B443B, 0xD53B441C) + tuple(
                            0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                        name = f"fp16binary_{op}_{shape}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                                               name, seeds, 0xB0000000, "fp16binary", bytes(range(64))))
    if len(cases) != 6080 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"FP16 binary coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, op, shape, alias_text, _, _ = case.name.split("_")
    rd, rn, rm = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]
    left, right, result, fpsr = vectors[rn], vectors[rm], 0, case.seeds[27]
    for lane in range(1 if shape == "s" else 4 if shape == "q0" else 8):
        value, flags = reference((left >> (lane * 16)) & 65535, (right >> (lane * 16)) & 65535, op, case.seeds[28])
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
            raise ValueError(f"{case.name}: independent FP16 rational arithmetic/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} FP16 binary/exact rational arithmetic/FABD/NaN/FZ16/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
