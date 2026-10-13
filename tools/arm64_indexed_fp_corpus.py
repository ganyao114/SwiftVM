"""Indexed H/S/D FMUL/FMULX/FMLA/FMLS, every legal element, full ELF snapshots."""

import argparse
import random
import struct

from arm64_fmulx_corpus import reference as extended_reference
from arm64_vector_fma_corpus import reference as fused_reference

OPS = ("fmul","fmulx","fmla","fmls")
ALIASES = ((0,1,2),(1,1,2),(2,1,2),(0,2,2),(2,2,2))
CONTROLS = (0,1 << 22,2 << 22,3 << 22,1 << 19,1 << 24,1 << 25,(1 << 19) | (1 << 24) | (1 << 25))
SHAPES = ((16,0,0),(16,1,0),(32,0,0),(32,1,0),(64,1,0),(16,1,1),(32,1,1),(64,1,1))
MASK64 = (1 << 64)-1


def reference(a,b,c,bits,operation,fpcr):
    if operation>=2:
        return fused_reference(a,b,c,bits,operation==3,fpcr)
    if operation==1:
        return extended_reference(a,b,bits,fpcr)
    fraction,bias = {16:10,32:23,64:52}[bits],{16:15,32:127,64:1023}[bits]
    sign,infinity,quiet = 1 << (bits-1),(2*bias+1) << fraction,1 << (fraction-1)
    values,flags = [a,b],0
    if fpcr & (1 << (19 if bits==16 else 24)):
        for slot,value in enumerate(values):
            if not value & infinity and value & ((1 << fraction)-1):
                values[slot] &= sign
                if bits!=16:
                    flags |= 128
    a,b = values
    if ((a & ~sign == infinity and not b & ~sign) or
        (b & ~sign == infinity and not a & ~sign)):
        return infinity | quiet,flags | 1
    value,exceptions = extended_reference(a,b,bits,fpcr)
    return value,flags | exceptions


def corpus(case_type):
    rng,cases = random.Random(0xF019DE2026),[]
    for bits,q,scalar in SHAPES:
        physical = (0,15,14,3) if bits==16 else (0,31,30,3)
        fraction,bias = {16:10,32:23,64:52}[bits],{16:15,32:127,64:1023}[bits]
        sign,infinity,quiet = 1 << (bits-1),(2*bias+1) << fraction,1 << (fraction-1)
        one,two = bias << fraction,(bias+1) << fraction
        for operation,op in enumerate(OPS):
            for index in range(128//bits):
                for alias,(rd,rn,rm) in enumerate(ALIASES):
                    for control,fpcr in enumerate(CONTROLS):
                        for profile in range(8):
                            vectors = [rng.getrandbits(128) for _ in range(4)]
                            variant = (alias+index)&3
                            b = (one-2,0,infinity,1,infinity | quiet | 5,one | (1 << (fraction-1)),two,rng.getrandbits(bits))[profile]
                            left,acc,element = [],[],[]
                            for lane in range(128//bits):
                                signed = sign if (lane+alias)&1 else 0
                                if profile==0:
                                    a,c = one+1,(sign if operation==2 else 0) | one
                                elif profile==1:
                                    a,c = signed | infinity,infinity | quiet | 3
                                elif profile==2:
                                    a,c = signed | (1 if variant&1 else 0),sign | one
                                elif profile==3:
                                    a,c = signed | 1,0
                                elif profile==4:
                                    a,c = ((infinity | quiet | 1,infinity | 2,sign | infinity | quiet | 3,infinity | 4)[(lane+alias)&3],
                                           (sign | infinity | quiet | 7,infinity | 8,one,sign | infinity | 9)[(lane+alias)&3])
                                elif profile==5:
                                    a,c = (one+1 if lane&1 else one+3),(sign | 1) if lane&1 else 1
                                elif profile==6:
                                    a,c = signed | (infinity-1),signed | (infinity-1)
                                else:
                                    a,c = rng.getrandbits(bits),rng.getrandbits(bits)
                                active = 1 if scalar else (128 if q else 64)//bits
                                if profile==3 and lane>=active:
                                    a,c = infinity | 1,infinity | 3
                                left.append(a);acc.append(c)
                                # Every unselected element is a signaling NaN.
                                element.append(b if lane==index else infinity | 11)
                            vectors[rd] = sum(value << (lane*bits) for lane,value in enumerate(acc))
                            vectors[rn] = sum(value << (lane*bits) for lane,value in enumerate(left))
                            vectors[rm] = sum(value << (lane*bits) for lane,value in enumerate(element))
                            seeds = {1+slot*2+part:(vector >> (part*64)) & MASK64
                                     for slot,vector in enumerate(vectors) for part in range(2)}
                            seeds.update({27:0x08000002,28:fpcr})
                            initialize = tuple(word for slot,reg in enumerate(physical) for word in
                                               (0x9E670000 | ((slot*2+1) << 5) | reg,0x9EAF0000 | ((slot*2+2) << 5) | reg))
                            base = (0x0F009000,0x2F009000,0x0F001000,0x0F005000)[operation]
                            word = base | ((0 if bits==16 else 2 if bits==32 else 3) << 22) | (q << 30) | (scalar << 28)
                            word |= (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                            if bits==16:
                                word |= ((index >> 2) << 11) | (((index >> 1)&1) << 21) | ((index&1) << 20)
                            elif bits==32:
                                word |= ((index >> 1) << 11) | ((index&1) << 21)
                            else:
                                word |= index << 11
                            capture = (0xD53B443B,0xD53B441C) + tuple(0x3D8003A0 | (slot << 10) | reg for slot,reg in enumerate(physical))
                            name = f"idxfp_{op}_e{bits}_q{q}_s{scalar}_i{index}_a{alias}_c{control}_p{profile}"
                            cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,
                                                   name,seeds,0xB0000000,"indexedfp",bytes(range(64))))
    if len(cases)!=51200 or len({case.name for case in cases})!=len(cases):
        raise ValueError(f"Indexed FP coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _,op,width,q_text,scalar_text,index_text,alias_text,_,_ = case.name.split("_")
    bits,q,scalar,index = int(width[1:]),int(q_text[1:]),int(scalar_text[1:]),int(index_text[1:])
    rd,rn,rm = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n+1] << 64) for n in (1,3,5,7)]
    left,right,acc,result,fpsr = vectors[rn],vectors[rm],vectors[rd],0,case.seeds[27]
    mask = (1 << bits)-1
    for lane in range(1 if scalar else (128 if q else 64)//bits):
        value,flags = reference((left >> (lane*bits)) & mask,(right >> (index*bits)) & mask,
                                (acc >> (lane*bits)) & mask,bits,OPS.index(op),case.seeds[28])
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
            raise ValueError(f"{case.name}: independent indexed FP rational arithmetic/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} H/S/D indexed scalar/vector FMUL/FMULX/FMLA/FMLS/exact integer arithmetic/NaN/FZ/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
