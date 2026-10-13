"""Independent GF(2) polynomial product and complete ELF Unicorn validation.

Standalone verification executes Unicorn only; it does not execute SwiftVM.
"""
import argparse
import random
import struct

OPS=("pmul","pmull")
SHAPES=((0,8,0),(0,8,1),(1,8,0),(1,8,1),(1,64,0),(1,64,1))
ALIASES=((0,1,2),(1,1,2),(2,1,2),(0,2,2),(2,2,2))
MASK64=(1<<64)-1

def encode(widening,bits,q,rd=0,rn=1,rm=2):
    return (0x0E20E000 if widening else 0x2E209C00)|((3 if bits==64 else 0)<<22)|(q<<30)|(rm<<16)|(rn<<5)|rd

def multiply(left,right):
    result=0
    while right:
        if right&1:result^=left
        right>>=1;left<<=1
    return result

def reference(left,right,widening,bits,q):
    if bits==64:return multiply((left>>(q*64))&MASK64,(right>>(q*64))&MASK64)
    result=0;start=8 if widening and q else 0;output_bits=16 if widening else 8
    for lane in range(8 if widening else 16 if q else 8):
        a,b=(left>>((start+lane)*8))&255,(right>>((start+lane)*8))&255
        result|=(multiply(a,b)&((1<<output_bits)-1))<<(lane*output_bits)
    return result

def corpus(case_type,family=None):
    rng,cases,physical=random.Random(0xF0020264),[],(0,31,30,3)
    edges=(0,1,2,1<<63,(1<<63)-1,MASK64,0xAAAAAAAAAAAAAAAA,0x5555555555555555)
    for widening,bits,q in SHAPES:
        op=OPS[widening]
        if family is not None and op!=family:continue
        for alias,(rd,rn,rm) in enumerate(ALIASES):
            for profile in range(72):
                vectors=[rng.getrandbits(128) for _ in range(4)]
                if profile<64:
                    if bits==64:
                        left=edges[profile//8]|(edges[(profile//8+3)%8]<<64)
                        right=edges[profile%8]|(edges[(profile%8+5)%8]<<64)
                    else:
                        left=sum(((profile*4+lane)&255)<<(8*lane) for lane in range(16))
                        right=sum(((profile*5+3*lane)&255)<<(8*lane) for lane in range(16))
                    vectors[rn]=left;vectors[rm]=right
                seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                seeds.update({27:0x0800009F if profile&1 else 0x91,28:0x03400000})
                initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                word=encode(widening,bits,q,physical[rd],physical[rn],physical[rm])
                capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                name=f"polynomial_{op}_e{bits}_q{q}_a{alias}_p{profile}"
                cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"polynomial",bytes(range(64))))
    count=2160 if family is None else 720 if family=="pmul" else 1440
    if len(cases)!=count or len({c.name for c in cases})!=len(cases):raise ValueError("Polynomial coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,atext,_=case.name.split("_")
    bits,q=int(width[1:]),int(qtext[1:]);widening=OPS.index(op)
    rd,rn,rm=ALIASES[int(atext[1:])]
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    vectors[rd]=reference(vectors[rn],vectors[rm],widening,bits,q)
    registers={f"x{n}":case.seeds.get(n,0x100+n) for n in range(29)}
    registers.update({"x29":d.MEMORY_BASE,"x30":d.DATA_BASE})
    return {"memory":b"".join(v.to_bytes(16,"little") for v in vectors),"registers":registers,"nzcv":case.nzcv,"fpsr":case.seeds[27],"fpcr":case.seeds[28]}

def verify(family=None):
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case,family)
    for case in cases:
        elf = differential.make_elf(case)
        code_segment = struct.unpack_from("<IIQQQQQQ", elf, 64)
        data_segment = struct.unpack_from("<IIQQQQQQ", elf, 120)
        code = elf[4096:code_segment[5]]
        decoded = list(decoder.disasm(code, differential.CODE_BASE + 4096))
        if len(decoded) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected ELF harness code")
        op = case.name.split("_")[1]
        if op=="pmull" and case.name.split("_")[3]=="q1":op="pmull2"
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
            raise ValueError(f"{case.name}: independent polynomial multiply/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} polynomial multiply/GF2/selected half/status/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--verify',action='store_true',required=True);parser.add_argument('--family',choices=OPS)
    args=parser.parse_args();raise SystemExit(verify(args.family))
