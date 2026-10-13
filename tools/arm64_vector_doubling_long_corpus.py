"""Vector saturating doubling long multiply, selected source halves and all indexed elements."""

import argparse
import random
import struct

OPS = ("sqdmull","sqdmlal","sqdmlsl")
ALIASES = ((0,1,2),(1,1,2),(2,1,2),(0,2,2),(2,2,2))
MASK64 = (1 << 64)-1


def signed(value,bits):
    value &= (1 << bits)-1
    return value-(1 << bits) if value & (1 << (bits-1)) else value


def reference(a,b,c,bits,operation):
    minimum,maximum = -(1 << (2*bits-1)),(1 << (2*bits-1))-1
    product = 2*signed(a,bits)*signed(b,bits)
    product_saturated = not minimum<=product<=maximum
    product = min(maximum,max(minimum,product))
    result = product if operation==0 else signed(c,2*bits)+product if operation==1 else signed(c,2*bits)-product
    accumulator_saturated = not minimum<=result<=maximum
    result = min(maximum,max(minimum,result))
    return result & ((1 << (2*bits))-1),product_saturated or accumulator_saturated


def corpus(case_type):
    rng,cases = random.Random(0xD0B110026),[]
    for bits in (16,32):
        sign,limit,mask = 1 << (bits-1),1 << (2*bits-1),(1 << bits)-1
        for upper in (0,1):
            for indexed in (0,1):
                physical = (0,31,14,3) if indexed and bits==16 else (0,31,30,3)
                for operation,op in enumerate(OPS):
                    for index in range(128//bits if indexed else 1):
                        for alias,(rd,rn,rm) in enumerate(ALIASES):
                            for sticky in (0,1):
                                for profile in range(10):
                                    vectors = [rng.getrandbits(128) for _ in range(4)]
                                    lhs,rhs,acc = [],[],[]
                                    for lane in range(128//bits):
                                        variant = (lane+alias)&3
                                        if profile==0:
                                            a,b = sign,sign
                                        elif profile==1:
                                            a,b = 1,1
                                        elif profile==2:
                                            a,b = mask,1
                                        elif profile==3:
                                            a,b = sign,sign-1
                                        elif profile==4:
                                            a,b = sign+1,sign+1
                                        elif profile==5:
                                            active = lane//(64//bits)==upper
                                            a,b = (0,0) if active else (sign,sign)
                                        else:
                                            a,b = rng.getrandbits(bits),rng.getrandbits(bits)
                                        lhs.append(a);rhs.append(b)
                                    for lane in range(64//bits):
                                        if profile==0:
                                            c = (MASK64,0,limit-1,limit)[(lane+alias)&3]
                                        elif profile==1:
                                            c = limit-1
                                        elif profile==2:
                                            c = limit
                                        elif profile==3:
                                            c = limit
                                        elif profile==4:
                                            c = MASK64
                                        elif profile==5:
                                            c = 0
                                        else:
                                            c = rng.getrandbits(2*bits)
                                        acc.append(c & ((1 << (2*bits))-1))
                                    if indexed:
                                        selected = (sign,1,1,sign-1,sign+1,0,rng.getrandbits(bits),rng.getrandbits(bits),rng.getrandbits(bits),rng.getrandbits(bits))[profile]
                                        rhs = [selected if lane==index else sign for lane in range(128//bits)]
                                    vectors[rd] = sum(value << (lane*2*bits) for lane,value in enumerate(acc))
                                    vectors[rn] = sum(value << (lane*bits) for lane,value in enumerate(lhs))
                                    vectors[rm] = sum(value << (lane*bits) for lane,value in enumerate(rhs))
                                    seeds = {1+slot*2+part:(vector >> (part*64)) & MASK64
                                             for slot,vector in enumerate(vectors) for part in range(2)}
                                    seeds.update({27:0x91 | (sticky << 27),28:0x03480000})
                                    initialize = tuple(word for slot,reg in enumerate(physical) for word in
                                                       (0x9E670000 | ((slot*2+1) << 5) | reg,0x9EAF0000 | ((slot*2+2) << 5) | reg))
                                    base = ((0x0F00B000,0x0F003000,0x0F007000) if indexed else (0x0E20D000,0x0E209000,0x0E20B000))[operation]
                                    word = base | ((1 if bits==16 else 2) << 22) | (upper << 30)
                                    word |= (physical[rm] << 16) | (physical[rn] << 5) | physical[rd]
                                    if indexed:
                                        word |= ((index >> (2 if bits==16 else 1)) << 11) | (((index >> (1 if bits==16 else 0))&1) << 21)
                                        if bits==16:
                                            word |= (index&1) << 20
                                    capture = (0xD53B443B,0xD53B441C) + tuple(0x3D8003A0 | (slot << 10) | reg for slot,reg in enumerate(physical))
                                    name = f"veclong_{op}_e{bits}_u{upper}_b{indexed}_i{index}_a{alias}_q{sticky}_p{profile}"
                                    cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,
                                                           name,seeds,0xB0000000,"veclong",bytes(range(64))))
    if len(cases)!=8400 or len({case.name for case in cases})!=len(cases):
        raise ValueError(f"Vector doubling-long coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _,op,width,upper_text,indexed_text,index_text,alias_text,_,_ = case.name.split("_")
    bits,upper,indexed,index = int(width[1:]),int(upper_text[1:]),int(indexed_text[1:]),int(index_text[1:])
    rd,rn,rm = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n+1] << 64) for n in (1,3,5,7)]
    left,right,acc,result,fpsr = vectors[rn],vectors[rm],vectors[rd],0,case.seeds[27]
    source_mask,destination_mask = (1 << bits)-1,(1 << (2*bits))-1
    for lane in range(64//bits):
        position = lane+upper*(64//bits)
        value,saturated = reference((left >> (position*bits)) & source_mask,
            (right >> ((index if indexed else position)*bits)) & source_mask,
            (acc >> (lane*2*bits)) & destination_mask,bits,OPS.index(op))
        result |= value << (lane*2*bits)
        if saturated: fpsr |= 1 << 27
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
        if "_u1_" in case.name: op += "2"
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
            raise ValueError(f"{case.name}: independent doubling-long BigInt arithmetic/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} vector SQDMULL/SQDMLAL/SQDMLSL/all-elements/product-first saturation/QC/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
