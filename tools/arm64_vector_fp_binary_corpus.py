"""S/D vector binary operations: exact rational arithmetic and full ELF oracle."""

import argparse
import random
import struct

from arm64_scalar_pairwise_fp_corpus import reference as pairwise_reference

OPS = ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fabd")
BASES = (0x0E20D400, 0x0EA0D400, 0x2E20DC00, 0x2E20FC00, 0x0E20F400,
         0x0EA0F400, 0x0E20C400, 0x0EA0C400, 0x2EA0D400)
ALIASES = ((0, 1, 2), (1, 1, 2), (2, 1, 2), (0, 2, 2), (2, 2, 2))
CONTROLS = (0, 1 << 22, 2 << 22, 3 << 22, 1 << 19, 1 << 25,
            (1 << 24) | (1 << 25), 1 << 24)
MASK64 = (1 << 64) - 1

def reference(a, b, bits, op, fpcr):
    fraction_bits = {32: 23, 64: 52}[bits]
    bias = {32: 127, 64: 1023}[bits]
    maximum_exponent = 2 * bias + 1
    exponent_mask = maximum_exponent << fraction_bits
    fraction_mask, quiet = (1 << fraction_bits) - 1, 1 << (fraction_bits - 1)
    sign_mask, magnitude_mask = 1 << (bits - 1), (1 << (bits - 1)) - 1
    scale, input_flags = fraction_bits + bias - 1, 0
    if op in ("fmax", "fmin", "fmaxnm", "fminnm"):
        return pairwise_reference(a, b, bits, op.replace("nm", "nmp") if "nm" in op else op + "p", fpcr)
    absolute = op == "fabd"
    if absolute:
        op = "fsub"

    def finish(value, flags):
        return value & magnitude_mask if absolute else value, flags | input_flags

    def nan(value):
        return value & exponent_mask == exponent_mask and bool(value & fraction_mask)

    def signaling(value):
        return nan(value) and not value & quiet

    if fpcr & (1 << 24):
        if not a & exponent_mask and a & fraction_mask:
            a &= sign_mask
            input_flags |= 128
        if not b & exponent_mask and b & fraction_mask:
            b &= sign_mask
            input_flags |= 128
    if nan(a) or nan(b):
        snan = signaling(a) or signaling(b)
        chosen = a if signaling(a) else b if signaling(b) else a if nan(a) else b
        return finish((exponent_mask | quiet) if fpcr & (1 << 25) else chosen | quiet, 1 if snan else 0)
    ainf, binf = a & magnitude_mask == exponent_mask, b & magnitude_mask == exponent_mask
    azero, bzero = not a & magnitude_mask, not b & magnitude_mask
    asign, bsign = bool(a & sign_mask), bool(b & sign_mask)
    mode = (fpcr >> 22) & 3

    def magnitude(value):
        exponent, fraction = (value >> fraction_bits) & maximum_exponent, value & fraction_mask
        return ((fraction | (1 << fraction_bits)) << (exponent - 1)) if exponent else fraction

    if op in ("fadd", "fsub"):
        if op == "fsub":
            bsign = not bsign
        if ainf or binf:
            if ainf and binf and asign != bsign:
                return finish((exponent_mask | quiet), 1)
            return finish((sign_mask if (asign if ainf else bsign) else 0) | exponent_mask, 0)
        numerator = (-magnitude(a) if asign else magnitude(a)) + (-magnitude(b) if bsign else magnitude(b))
        denominator = 1 << scale
        if not numerator:
            sign = asign if asign == bsign else mode == 2
            return finish(sign_mask if sign else 0, 0)
        negative = numerator < 0
        numerator = abs(numerator)
    else:
        negative = asign != bsign
        if op == "fmul":
            if (ainf and bzero) or (binf and azero):
                return finish((exponent_mask | quiet), 1)
            if ainf or binf:
                return finish((sign_mask if negative else 0) | exponent_mask, 0)
            numerator, denominator = magnitude(a) * magnitude(b), 1 << (2 * scale)
        else:
            if (azero and bzero) or (ainf and binf):
                return finish((exponent_mask | quiet), 1)
            if ainf or bzero:
                return finish((sign_mask if negative else 0) | exponent_mask, 2 if bzero and not ainf else 0)
            if binf:
                return finish(sign_mask if negative else 0, 0)
            numerator, denominator = magnitude(a), magnitude(b)
    sign = sign_mask if negative else 0
    if not numerator:
        return finish(sign, 0)
    numerator <<= scale  # Rational in minimum-subnormal units.
    tiny = numerator < denominator * (1 << fraction_bits)
    if tiny and fpcr & (1 << 24):
        return finish(sign, 8)
    top = numerator.bit_length() - denominator.bit_length()
    if (numerator < denominator << top) if top >= 0 else (numerator << -top < denominator):
        top -= 1
    shift = max(0, top - fraction_bits)
    rounded, remainder = divmod(numerator, denominator << shift)
    flags = 0
    if remainder:
        flags |= 16 | (8 if tiny else 0)
        increment = (mode == 1 and not negative) or (mode == 2 and negative)
        if mode == 0:
            midpoint = denominator << shift
            increment = 2 * remainder > midpoint or (2 * remainder == midpoint and bool(rounded & 1))
        rounded += increment
    if rounded >= (1 << (fraction_bits + 1)):
        rounded >>= 1
        shift += 1
    exponent = 0 if rounded < (1 << fraction_bits) else shift + 1
    if exponent >= maximum_exponent:
        infinity = mode == 0 or (mode == 1 and not negative) or (mode == 2 and negative)
        return finish(sign | (exponent_mask if infinity else (exponent_mask - 1)), flags | 20)
    return finish(sign | (exponent << fraction_bits) | (rounded & fraction_mask), flags)


def corpus(case_type):
    rng, cases, physical = random.Random(0xF0B1A420262), [], (0, 31, 30, 3)
    for bits, q in ((32, 0), (32, 1), (64, 1)):
        fraction, bias = {32: 23, 64: 52}[bits], {32: 127, 64: 1023}[bits]
        sign, infinity, quiet = 1 << (bits - 1), (2 * bias + 1) << fraction, 1 << (fraction - 1)
        one, two, three, half_ulp = bias << fraction, (bias + 1) << fraction, ((bias + 1) << fraction) | (1 << (fraction - 1)), (bias - fraction - 1) << fraction
        minnormal = 1 << fraction
        profiles = (
            ((infinity | quiet | 3,infinity | 5,sign | infinity | quiet | 7,sign | infinity | 9),
             (infinity | 1,infinity | quiet | 9,sign | infinity | 3,sign | infinity | quiet | 3)),
            ((0,sign,infinity,sign | infinity),(sign,0,sign | infinity,infinity)),
            ((one,sign | one,one + 1,sign | (one + 1)),(half_ulp,half_ulp,half_ulp,half_ulp)),
            ((1,sign | 1,minnormal,minnormal | sign),(one,one,three,three)),
            ((infinity - 1,sign | (infinity - 1),infinity - 2,sign | (infinity - 2)),
             (two,two,infinity - 1,sign | (infinity - 1))),
            ((0,0,infinity | 1,sign | infinity),(one,one,infinity | 3,infinity)),
            ((minnormal + 1,minnormal - 1,one + 1,sign | one),(sign | minnormal,1,three,one + 1)),
        )
        for op_index, op in enumerate(OPS):
            for alias, (rd, rn, rm) in enumerate(ALIASES):
                for control, fpcr in enumerate(CONTROLS):
                    for profile in range(8):
                        vectors = [rng.getrandbits(128) for _ in range(4)]
                        if profile:
                            left, right = profiles[profile - 1]
                            offset = 2 if bits == 64 and alias & 1 else 0
                            left = left[offset:offset + 128 // bits]
                            right = right[offset:offset + 128 // bits]
                        else:
                            left = tuple(rng.getrandbits(bits) for _ in range(128 // bits))
                            right = tuple(rng.getrandbits(bits) for _ in range(128 // bits))
                        vectors[rn] = sum(value << (lane * bits) for lane, value in enumerate(left))
                        vectors[rm] = sum(value << (lane * bits) for lane, value in enumerate(right))
                        seeds = {1 + slot * 2 + part: (vector >> (part * 64)) & MASK64
                                 for slot, vector in enumerate(vectors) for part in range(2)}
                        seeds.update({27: 0x08000002, 28: fpcr})
                        initialize = tuple(word for slot, reg in enumerate(physical) for word in
                                           (0x9E670000 | ((slot * 2 + 1) << 5) | reg,
                                            0x9EAF0000 | ((slot * 2 + 2) << 5) | reg))
                        word = BASES[op_index] | (q << 30) | ((1 << 22) if bits == 64 else 0)
                        word |= (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                        capture = (0xD53B443B, 0xD53B441C) + tuple(
                            0x3D8003A0 | (slot << 10) | reg for slot, reg in enumerate(physical))
                        name = f"vecfp_{op}_e{bits}_q{q}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name, (0xD51B443B, 0xD51B441C) + initialize + (word,) + capture,
                                               name, seeds, 0xB0000000, "vecfp", bytes(range(64))))
    if len(cases) != 8640 or len({case.name for case in cases}) != len(cases):
        raise ValueError(f"S/D vector FP binary coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _, op, width, q_text, alias_text, _, _ = case.name.split("_")
    bits, q = int(width[1:]), int(q_text[1:])
    rd, rn, rm = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n + 1] << 64) for n in (1, 3, 5, 7)]
    left, right, result, fpsr = vectors[rn], vectors[rm], 0, case.seeds[27]
    mask = (1 << bits) - 1
    for lane in range((128 if q else 64) // bits):
        value, flags = reference((left >> (lane * bits)) & mask, (right >> (lane * bits)) & mask, bits, op, case.seeds[28])
        result |= value << (lane * bits)
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
            raise ValueError(f"{case.name}: independent S/D rational arithmetic/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} S/D vector FP binary/exact rational arithmetic/FABD/NaN/FZ/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
