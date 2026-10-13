"""Independent exact mixed-sign saturation/unary oracle; standalone runs only Unicorn."""
import argparse
import random
import struct

OPS=("suqadd","usqadd","sqabs","sqneg")
BASES=(0x0E203800,0x2E203800,0x0E207800,0x2E207800)
SHAPES=tuple((bits,q,0) for bits in (8,16,32,64) for q in (0,1) if bits!=64 or q)+tuple((bits,1,1) for bits in (8,16,32,64))
ALIASES=((0,1),(1,1),(2,1))
MASK64=(1<<64)-1

def encode(bits,q,scalar,operation,rd=0,rn=1):
    return BASES[operation]|((bits.bit_length()-4)<<22)|(q<<30)|(scalar<<28)|(rn<<5)|rd

def reference(source,accumulator,bits,operation):
    sign=1<<(bits-1);mask=(1<<bits)-1
    signed=lambda value:value-(1<<bits) if value&sign else value
    value=(signed(accumulator)+source if operation==0 else accumulator+signed(source) if operation==1 else abs(signed(source)) if operation==2 else -signed(source))
    low,high=(0,mask) if operation==1 else (-sign,sign-1)
    return min(high,max(low,value))&mask,int(value<low or value>high)<<27

def edges(bits):
    sign=1<<(bits-1);mask=(1<<bits)-1
    return (0,1,2,sign-2,sign-1,sign,sign+1,sign+2,mask-2,mask-1,mask,0xAAAAAAAAAAAAAAAA&mask,0x5555555555555555&mask)

def corpus(case_type,family=None):
    rng,cases,physical=random.Random(0x5A7020264),[],(0,31,30,3)
    for bits,q,scalar in SHAPES:
        for operation,op in enumerate(OPS):
            if scalar and operation>=2 or family is not None and op!=family:continue
            values=edges(bits)
            for alias,(rd,rn) in enumerate(ALIASES):
                for profile in range(185):
                    vectors=[rng.getrandbits(128) for _ in range(4)]
                    if profile<169:
                        vectors[rd]=sum(values[(profile//13+lane)%13]<<(bits*lane) for lane in range(128//bits))
                        vectors[rn]=sum(values[(profile%13+lane*3)%13]<<(bits*lane) for lane in range(128//bits))
                    if profile==169:
                        active=1 if scalar else (128 if q else 64)//bits
                        vectors[rd]=0
                        vectors[rn]=sum((0 if lane<active else 1<<(bits-1))<<(bits*lane) for lane in range(128//bits))
                    seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                    seeds.update({27:0x0800009F if profile&1 else 0x91,28:0x03400000})
                    initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                    word=encode(bits,q,scalar,operation,physical[rd],physical[rn])
                    capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                    name=f"mixedsat_{op}_e{bits}_q{q}_s{scalar}_a{alias}_p{profile}"
                    cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"mixedsat",bytes(range(64))))
    count=19980 if family is None else 6105 if OPS.index(family)<2 else 3885
    if len(cases)!=count or len({c.name for c in cases})!=len(cases):raise ValueError("Mixed saturation coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,stext,atext,_=case.name.split("_")
    bits,q,scalar=int(width[1:]),int(qtext[1:]),int(stext[1:]);operation=OPS.index(op)
    rd,rn=ALIASES[int(atext[1:])];mask=(1<<bits)-1
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    result=0;flags=case.seeds[27]
    for lane in range(1 if scalar else (128 if q else 64)//bits):
        value,exc=reference((vectors[rn]>>(bits*lane))&mask,(vectors[rd]>>(bits*lane))&mask,bits,operation)
        result|=value<<(bits*lane);flags|=exc
    vectors[rd]=result
    registers={f"x{n}":case.seeds.get(n,0x100+n) for n in range(29)}
    registers.update({"x27":flags,"x29":d.MEMORY_BASE,"x30":d.DATA_BASE})
    return {"memory":b"".join(v.to_bytes(16,"little") for v in vectors),"registers":registers,"nzcv":case.nzcv,"fpsr":flags,"fpcr":case.seeds[28]}

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
            raise ValueError(f"{case.name}: independent mixed/unary saturation/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} mixed/unary saturation/signed range/QC/status/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--verify',action='store_true',required=True);parser.add_argument('--family',choices=OPS)
    args=parser.parse_args();raise SystemExit(verify(args.family))
