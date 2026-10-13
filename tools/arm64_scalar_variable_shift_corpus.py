"""Scalar variable shifts: exact integer oracle and standalone Unicorn fixtures.

Standalone --verify validates the reference and ELF harness, not SwiftVM.
"""
import argparse
import random
import struct

OPS=("sshl","ushl","srshl","urshl","sqshl","uqshl","sqrshl","uqrshl")
BASES=(0x5E204400,0x7E204400,0x5E205400,0x7E205400,
       0x5E204C00,0x7E204C00,0x5E205C00,0x7E205C00)
# Slots map to physical V0,V30,V31,V3. Five distinct alias relationships.
ALIASES=((0,1,2),(1,1,2),(2,1,2),(0,2,2),(2,2,2))
PHYSICAL=(0,30,31,3)
CONTROLS=(0,1<<19,3<<22,1<<24,1<<25,1<<26)
MASK64=(1<<64)-1

def encode(bits,operation,rd=0,rn=30,rm=31):
    return BASES[operation]|((bits.bit_length()-4)<<22)|(rm<<16)|(rn<<5)|rd

def reference(value,raw_count,bits,operation):
    mask=(1<<bits)-1
    value &= mask
    unsigned=bool(operation&1)
    if not unsigned and value&(1<<(bits-1)):value-=1<<bits
    count=raw_count&255
    if count>=128:count-=256
    saturated=False
    if count>=0:
        value*=1<<count
        if operation>=4:
            lo,hi=(0,mask) if unsigned else (-(1<<(bits-1)),(1<<(bits-1))-1)
            saturated=not lo<=value<=hi
            value=min(hi,max(lo,value))
    else:
        divisor=1<<(-count)
        if operation&2:value+=divisor//2
        value//=divisor  # Python // is mathematical floor, including negatives.
    return value&mask,(1<<27) if saturated else 0

def edges(bits):
    sign=1<<(bits-1);mask=(1<<bits)-1
    return (0,1,2,3,sign-1,sign,sign+1,mask,mask-1,mask-2,sign//2,sign//2-1)

def corpus(case_type,family=None):
    rng,cases=random.Random(0x5CA1A8256127),[]
    for operation,op in enumerate(OPS):
        if family is not None and op!=family:continue
        for bits in ((64,) if operation<4 else (8,16,32,64)):
            values=edges(bits)
            boundary=(-128,-bits-1,-bits,1-bits,-2,-1,0,1,2,bits-1,bits,bits+1,127)
            for alias,(rd,rn,rm) in enumerate(ALIASES):
                for profile in range(304):
                    count=profile-128 if profile<256 else boundary[(profile-256)%len(boundary)] if profile<280 else rng.randrange(-128,128)
                    value=values[profile%len(values)] if profile<280 else rng.getrandbits(bits)
                    vectors=[rng.getrandbits(128) for _ in range(4)]
                    # Both operands have poisoned inactive lanes; count uses only low byte.
                    vectors[rn]=(vectors[rn]&~MASK64)|(value|(MASK64^((1<<bits)-1)))
                    vectors[rm]=(vectors[rm]&~255)|(count&255)
                    sticky=0x0800009F if profile&1 else 0x9F
                    fpcr=CONTROLS[profile%len(CONTROLS)]
                    seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                    seeds.update({27:sticky,28:fpcr})
                    initialize=tuple(word for slot,reg in enumerate(PHYSICAL) for word in
                        (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                    word=encode(bits,operation,PHYSICAL[rd],PHYSICAL[rn],PHYSICAL[rm])
                    capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(PHYSICAL))
                    name=f"scalarvarshift_{op}_e{bits}_a{alias}_p{profile}"
                    cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,
                                           name,seeds,0xB0000000,"scalarvarshift",bytes(range(64))))
    count=30400 if family is None else (1520 if OPS.index(family)<4 else 6080)
    if len(cases)!=count or len({case.name for case in cases})!=len(cases):
        raise ValueError("scalar variable shift coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,alias,_=case.name.split("_")
    bits,operation=int(width[1:]),OPS.index(op)
    rd,rn,rm=ALIASES[int(alias[1:])]
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    value,exception=reference(vectors[rn],vectors[rm],bits,operation)
    vectors[rd]=value
    flags=case.seeds[27]|exception
    registers={f"x{n}":case.seeds.get(n,0x100+n) for n in range(29)}
    registers.update({"x27":flags,"x28":case.seeds[28],"x29":d.MEMORY_BASE,"x30":d.DATA_BASE})
    return {"memory":b"".join(v.to_bytes(16,"little") for v in vectors),"registers":registers,
            "nzcv":case.nzcv,"fpsr":flags,"fpcr":case.seeds[28]}

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
            raise ValueError(f"{case.name}: independent scalar variable shift/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} scalar variable shift/full ELF/Capstone/Unicorn reference cases (not SwiftVM execution)")
    return 0



if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify",action="store_true")
    parser.add_argument("--family",choices=OPS)
    args=parser.parse_args()
    if args.verify:verify(args.family)
    else:parser.error("choose --verify")
