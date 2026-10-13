"""Scalar/vector H/S/D FRECPS/FRSQRTS: exact fused rational arithmetic and full ELF snapshots."""

import argparse
import random
import struct

ALIASES = ((0,1,2),(1,1,2),(2,1,2),(0,2,2),(2,2,2))
CONTROLS = (0,1 << 22,2 << 22,3 << 22,1 << 19,1 << 24,1 << 25,(1 << 19) | (1 << 24) | (1 << 25))
FORMS = tuple((bits,q,scalar,rsqrt) for bits,q,scalar in ((16,0,0),(16,1,0),(32,0,0),(32,1,0),(64,1,0),(16,1,1),(32,1,1),(64,1,1)) for rsqrt in (0,1))
MASK64 = (1 << 64)-1


def reference(a,b,bits,rsqrt,fpcr):
    fraction,bias = {16:10,32:23,64:52}[bits],{16:15,32:127,64:1023}[bits]
    sign,exponent_mask,quiet = 1 << (bits-1),(2*bias+1) << fraction,1 << (fraction-1)
    fraction_mask,scale,flags = (1 << fraction)-1,fraction+bias-1,0
    flush = fpcr & (1 << (19 if bits==16 else 24))
    values = [a,b]
    if flush:
        for index,value in enumerate(values):
            if not value & exponent_mask and value & fraction_mask:
                values[index] &= sign
                if bits!=16:
                    flags |= 128
    a,b = values
    a ^= sign  # Negate before ARM NaN selection.

    def nan(value):
        return value & exponent_mask == exponent_mask and bool(value & fraction_mask)

    chosen = next((value for value in (a,b) if nan(value) and not value & quiet),None)
    if chosen is not None:
        return exponent_mask | quiet if fpcr & (1 << 25) else chosen | quiet,flags | 1
    chosen = next((value for value in (a,b) if nan(value)),None)
    if chosen is not None:
        return exponent_mask | quiet if fpcr & (1 << 25) else chosen | quiet,flags
    negative = bool(a & sign) != bool(b & sign)
    result_sign = sign if negative else 0
    ainf,binf = a & ~sign == exponent_mask,b & ~sign == exponent_mask
    if (ainf and not b & ~sign) or (binf and not a & ~sign):
        return ((bias << fraction) | (1 << (fraction-1))) if rsqrt else ((bias+1) << fraction),flags
    if ainf or binf:
        return result_sign | exponent_mask,flags

    def magnitude(value):
        exponent,significand = (value & exponent_mask) >> fraction,value & fraction_mask
        return ((significand | (1 << fraction)) << (exponent-1)) if exponent else significand

    product = magnitude(a)*magnitude(b)
    numerator = ((3 if rsqrt else 2) << (2*scale))+(-product if negative else product)
    if not numerator:
        return sign if ((fpcr >> 22)&3)==2 else 0,flags
    negative,numerator = numerator<0,abs(numerator)
    result_sign = sign if negative else 0
    denominator = 1 << (scale+rsqrt)
    tiny = numerator < denominator*(1 << fraction)
    if tiny and flush:
        return result_sign,flags | 8
    top = numerator.bit_length()-denominator.bit_length()
    below = numerator < denominator << top if top>=0 else numerator << -top < denominator
    if below:
        top -= 1
    shift = max(0,top-fraction)
    divisor = denominator << shift
    rounded,remainder = divmod(numerator,divisor)
    mode = (fpcr >> 22)&3
    if remainder:
        flags |= 16 | (8 if tiny else 0)
        increment = (mode==1 and not negative) or (mode==2 and negative)
        if mode==0:
            increment = 2*remainder>divisor or (2*remainder==divisor and bool(rounded&1))
        rounded += increment
    if rounded >= 1 << (fraction+1):
        rounded >>= 1
        shift += 1
    exponent = 0 if rounded < 1 << fraction else shift+1
    if exponent>=2*bias+1:
        infinity = mode==0 or (mode==1 and not negative) or (mode==2 and negative)
        return result_sign | (exponent_mask if infinity else exponent_mask-1),flags | 20
    return result_sign | (exponent << fraction) | (rounded & fraction_mask),flags


def corpus(case_type):
    rng,cases,physical = random.Random(0xF0A1E2026),[],(0,31,30,3)
    for bits,q,scalar,rsqrt in FORMS:
        fraction,bias = {16:10,32:23,64:52}[bits],{16:15,32:127,64:1023}[bits]
        sign,infinity,quiet = 1 << (bits-1),(2*bias+1) << fraction,1 << (fraction-1)
        one,two,minnormal = bias << fraction,(bias+1) << fraction,1 << fraction
        three = two | (1 << (fraction-1))
        for alias,(rd,rn,rm) in enumerate(ALIASES):
            for control,fpcr in enumerate(CONTROLS):
                for profile in range(12):
                    vectors = [rng.getrandbits(128) for _ in range(4)]
                    left,right = [],[]
                    for lane in range(128//bits):
                        variant = (lane+alias)&3
                        signed = sign if variant&1 else 0
                        if profile==0:
                            a,b = signed,(sign if variant&2 else 0) | infinity
                        elif profile==1:
                            a,b = signed | infinity,sign if variant&2 else 0
                        elif profile==2:
                            a,b = signed | 1,(sign if variant&2 else 0) | infinity
                        elif profile==3:
                            a,b = signed | infinity,(sign if variant&2 else 0) | ((1 << fraction)-1)
                        elif profile==4:
                            a,b = ((infinity | quiet | 1,infinity | 2,sign | infinity | quiet | 3,infinity | 4)[variant],
                                   (sign | infinity | 5,infinity | quiet | 6,infinity | quiet | 7,sign | infinity | 8)[variant])
                        elif profile==5:
                            a,b = signed | (one+1),one | (1 << (fraction-1))
                        elif profile==6:
                            a,b = (one+2,three-3) if rsqrt else (one+1,two-2)
                        elif profile==7:
                            a,b = signed | (infinity-1),two
                        elif profile==8:
                            a,b = signed | 1,(bias-1) << fraction
                        elif profile==9:
                            active = 1 if scalar else (128 if q else 64)//bits
                            a,b = (one,one) if lane<active else (infinity | 1,0)
                        elif profile==10:
                            a,b = signed | 1,1
                        else:
                            a,b = rng.getrandbits(bits),rng.getrandbits(bits)
                        left.append(a);right.append(b)
                    vectors[rn] = sum(value << (lane*bits) for lane,value in enumerate(left))
                    vectors[rm] = sum(value << (lane*bits) for lane,value in enumerate(right))
                    seeds = {1+slot*2+part:(vector >> (part*64)) & MASK64
                             for slot,vector in enumerate(vectors) for part in range(2)}
                    seeds.update({27:0x08000002,28:fpcr})
                    initialize = tuple(word for slot,reg in enumerate(physical) for word in
                                       (0x9E670000 | ((slot*2+1) << 5) | reg,0x9EAF0000 | ((slot*2+2) << 5) | reg))
                    word = (0x0E403C00 if bits==16 else 0x0E20FC00) | (q << 30) | (scalar << 28) | (rsqrt << 23)
                    word |= ((1 << 22) if bits==64 else 0) | (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                    capture = (0xD53B443B,0xD53B441C) + tuple(0x3D8003A0 | (slot << 10) | reg for slot,reg in enumerate(physical))
                    name = f"recipstep_{'frsqrts' if rsqrt else 'frecps'}_e{bits}_q{q}_s{scalar}_a{alias}_c{control}_p{profile}"
                    cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,
                                           name,seeds,0xB0000000,"recipstep",bytes(range(64))))
    if len(cases)!=7680 or len({case.name for case in cases})!=len(cases):
        raise ValueError(f"Reciprocal step coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _,op,width,q_text,scalar_text,alias_text,_,_ = case.name.split("_")
    bits,q,scalar = int(width[1:]),int(q_text[1:]),int(scalar_text[1:])
    rd,rn,rm = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n+1] << 64) for n in (1,3,5,7)]
    left,right,result,fpsr = vectors[rn],vectors[rm],0,case.seeds[27]
    mask = (1 << bits)-1
    for lane in range(1 if scalar else (128 if q else 64)//bits):
        value,flags = reference((left >> (lane*bits)) & mask,(right >> (lane*bits)) & mask,bits,op=="frsqrts",case.seeds[28])
        result |= value << (lane*bits);fpsr |= flags
    vectors[rd] = result
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
            raise ValueError(f"{case.name}: independent reciprocal-step rational arithmetic/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} H/S/D scalar/vector FRECPS/FRSQRTS/exact fused integer arithmetic/NaN/FZ/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
