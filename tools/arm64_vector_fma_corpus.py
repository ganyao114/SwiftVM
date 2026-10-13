"""H/S/D vector FMLA/FMLS: exact fused integer arithmetic and full ELF snapshots."""

import argparse
import random
import struct

OPS = ("fmla", "fmls")
ALIASES = ((0,1,2),(1,1,2),(2,1,2),(0,2,2),(2,2,2))
CONTROLS = (0,1 << 22,2 << 22,3 << 22,1 << 19,1 << 24,1 << 25,(1 << 19) | (1 << 24) | (1 << 25))
MASK64 = (1 << 64)-1


def reference(a,b,c,bits,subtract,fpcr):
    fraction,bias = {16:10,32:23,64:52}[bits],{16:15,32:127,64:1023}[bits]
    max_exponent,sign = 2*bias+1,1 << (bits-1)
    exponent_mask,fraction_mask,quiet = max_exponent << fraction,(1 << fraction)-1,1 << (fraction-1)
    scale,flags = fraction+bias-1,0
    flush = fpcr & (1 << (19 if bits==16 else 24))
    values = [a,b,c]
    if flush:
        for index,value in enumerate(values):
            if not value & exponent_mask and value & fraction_mask:
                values[index] &= sign
                if bits!=16:
                    flags |= 128
    a,b,c = values
    if subtract:
        a ^= sign

    def nan(value):
        return value & exponent_mask == exponent_mask and bool(value & fraction_mask)

    def signaling(value):
        return nan(value) and not value & quiet

    ordered = (c,a,b)
    selected = next((value for value in ordered if signaling(value)),None)
    invalid_product = ((a & ~sign == exponent_mask and not b & ~sign) or
                       (b & ~sign == exponent_mask and not a & ~sign))
    if invalid_product and selected is None:
        return exponent_mask | quiet,flags | 1
    if selected is not None:
        return exponent_mask | quiet if fpcr & (1 << 25) else selected | quiet,flags | 1
    selected = next((value for value in ordered if nan(value)),None)
    if selected is not None:
        return exponent_mask | quiet if fpcr & (1 << 25) else selected | quiet,flags
    product_negative,acc_negative = bool(a & sign) != bool(b & sign),bool(c & sign)
    product_inf = a & ~sign == exponent_mask or b & ~sign == exponent_mask
    acc_inf = c & ~sign == exponent_mask
    if product_inf or acc_inf:
        if product_inf and acc_inf and product_negative != acc_negative:
            return exponent_mask | quiet,flags | 1
        negative = product_negative if product_inf else acc_negative
        return (sign if negative else 0) | exponent_mask,flags

    def magnitude(value):
        exponent,significand = (value & exponent_mask) >> fraction,value & fraction_mask
        return ((significand | (1 << fraction)) << (exponent-1)) if exponent else significand

    product,addend = magnitude(a)*magnitude(b),magnitude(c) << scale
    numerator = (-product if product_negative else product)+(-addend if acc_negative else addend)
    mode = (fpcr >> 22)&3
    if not numerator:
        negative = product_negative if product_negative==acc_negative else mode==2
        return sign if negative else 0,flags
    negative,numerator = numerator<0,abs(numerator)
    result_sign = sign if negative else 0
    numerator <<= scale
    denominator = 1 << (2*scale)
    tiny = numerator < denominator*(1 << fraction)
    if tiny and flush:
        return result_sign,flags | 8
    top = numerator.bit_length()-denominator.bit_length()
    below = numerator < denominator << top if top>=0 else numerator << -top < denominator
    if below:
        top -= 1
    shift = max(0,top-fraction)
    rounded,remainder = divmod(numerator,denominator << shift)
    if remainder:
        flags |= 16 | (8 if tiny else 0)
        increment = (mode==1 and not negative) or (mode==2 and negative)
        if mode==0:
            midpoint = denominator << shift
            increment = 2*remainder>midpoint or (2*remainder==midpoint and bool(rounded&1))
        rounded += increment
    if rounded >= 1 << (fraction+1):
        rounded >>= 1
        shift += 1
    exponent = 0 if rounded < 1 << fraction else shift+1
    if exponent>=max_exponent:
        infinity = mode==0 or (mode==1 and not negative) or (mode==2 and negative)
        return result_sign | (exponent_mask if infinity else exponent_mask-1),flags | 20
    return result_sign | (exponent << fraction) | (rounded & fraction_mask),flags


def corpus(case_type):
    rng,cases,physical = random.Random(0xF0A10A42026),[],(0,31,30,3)
    for bits,q in ((16,0),(16,1),(32,0),(32,1),(64,1)):
        fraction,bias = {16:10,32:23,64:52}[bits],{16:15,32:127,64:1023}[bits]
        sign,infinity,quiet = 1 << (bits-1),(2*bias+1) << fraction,1 << (fraction-1)
        one,two,three = bias << fraction,(bias+1) << fraction,((bias+1) << fraction) | (1 << (fraction-1))
        half_ulp,minnormal = (bias-fraction-1) << fraction,1 << fraction
        for operation,op in enumerate(OPS):
            for alias,(rd,rn,rm) in enumerate(ALIASES):
                for control,fpcr in enumerate(CONTROLS):
                    for profile in range(10):
                        vectors = [rng.getrandbits(128) for _ in range(4)]
                        left,right,acc = [],[],[]
                        for lane in range(128//bits):
                            variant = (lane+alias)&3
                            if profile==0:
                                a,b,c = one+1,one-2,(sign if operation==0 else 0) | one
                            elif profile==1:
                                a,b,c = ((infinity | quiet | 1,infinity | 2,sign | infinity | quiet | 3,infinity | 4)[variant],
                                         (sign | infinity | 5,infinity | quiet | 6,infinity | 7,sign | infinity | quiet | 8)[variant],
                                         (infinity | quiet | 9,sign | infinity | 10,one,sign | infinity | quiet | 11)[variant])
                            elif profile==2:
                                a,b,c = ((infinity,0,infinity,sign | infinity)[variant],
                                         (0,infinity,one,sign | one)[variant],
                                         (infinity | quiet | 3,sign | infinity | 4,sign | infinity,infinity)[variant])
                            elif profile==3:
                                a,b,c = (sign if variant&1 else 0),one,(sign if variant&2 else 0)
                            elif profile==4:
                                a,b,c = one,half_ulp,((one if operation==0 else sign | one)+(variant&1))
                                if variant>=2:
                                    # A midpoint product with a correction below
                                    # binary128 precision exercises round-to-odd.
                                    a,b,c = (one+1 if variant==2 else one+3),one | (1 << (fraction-1)),(sign | 1) if variant==2 else 1
                            elif profile==5:
                                a,b,c = minnormal+1,one-1,(sign if operation==0 else 0) | minnormal
                                if variant&1:
                                    a,b,c = 1,three,(sign if operation==0 else 0) | 1
                            elif profile==6:
                                a,b,c = infinity-1,two,(sign if variant&1 else 0) | (infinity-1)
                            elif profile==7:
                                active = (128 if q else 64)//bits
                                a,b,c = (0,one,0) if lane<active else (infinity | 1,0,infinity | 3)
                            else:
                                a,b,c = rng.getrandbits(bits),rng.getrandbits(bits),rng.getrandbits(bits)
                            left.append(a);right.append(b);acc.append(c)
                        vectors[rd] = sum(value << (lane*bits) for lane,value in enumerate(acc))
                        vectors[rn] = sum(value << (lane*bits) for lane,value in enumerate(left))
                        vectors[rm] = sum(value << (lane*bits) for lane,value in enumerate(right))
                        seeds = {1+slot*2+part:(vector >> (part*64)) & MASK64
                                 for slot,vector in enumerate(vectors) for part in range(2)}
                        seeds.update({27:0x08000002,28:fpcr})
                        initialize = tuple(word for slot,reg in enumerate(physical) for word in
                                           (0x9E670000 | ((slot*2+1) << 5) | reg,0x9EAF0000 | ((slot*2+2) << 5) | reg))
                        word = (0x0E400C00 if bits==16 else 0x0E20CC00) | (operation << 23) | (q << 30)
                        word |= ((1 << 22) if bits==64 else 0) | (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                        capture = (0xD53B443B,0xD53B441C) + tuple(0x3D8003A0 | (slot << 10) | reg for slot,reg in enumerate(physical))
                        name = f"vecfma_{op}_e{bits}_q{q}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,
                                               name,seeds,0xB0000000,"vecfma",bytes(range(64))))
    if len(cases)!=4000 or len({case.name for case in cases})!=len(cases):
        raise ValueError(f"Vector FMA coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _,op,width,q_text,alias_text,_,_ = case.name.split("_")
    bits,q = int(width[1:]),int(q_text[1:])
    rd,rn,rm = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n+1] << 64) for n in (1,3,5,7)]
    left,right,acc,result,fpsr = vectors[rn],vectors[rm],vectors[rd],0,case.seeds[27]
    mask = (1 << bits)-1
    for lane in range((128 if q else 64)//bits):
        value,flags = reference((left >> (lane*bits)) & mask,(right >> (lane*bits)) & mask,
                                (acc >> (lane*bits)) & mask,bits,op=="fmls",case.seeds[28])
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
            raise ValueError(f"{case.name}: independent vector fused integer arithmetic/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} H/S/D vector fused multiply/exact integer arithmetic/NaN/FZ/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
