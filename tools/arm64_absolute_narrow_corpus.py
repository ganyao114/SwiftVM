"""Exact absolute narrow difference/wrapping oracle; standalone executes only Unicorn."""
import argparse
import random
import struct

OPS=("sabd","uabd","saba","uaba")
BASES=(0x0E207400,0x2E207400,0x0E207C00,0x2E207C00)
ALIASES=((0,1,2),(1,1,2),(2,1,2),(0,2,2),(2,2,2))
MASK64=(1<<64)-1

def encode(bits,q,operation,rd=0,rn=1,rm=2):
    return BASES[operation]|((bits.bit_length()-4)<<22)|(q<<30)|(rm<<16)|(rn<<5)|rd

def reference(left,right,accumulator,bits,operation,q):
    mask=(1<<bits)-1;output_bits=bits;result=0;first=0
    signed=lambda value:value-(1<<bits) if value&(1<<(bits-1)) else value
    for lane in range((128 if q else 64)//bits):
        a,b=(left>>((first+lane)*bits))&mask,(right>>((first+lane)*bits))&mask
        value=abs(a-b) if operation&1 else abs(signed(a)-signed(b))
        if operation>=2:value+=(accumulator>>(lane*output_bits))&((1<<output_bits)-1)
        result|=(value&((1<<output_bits)-1))<<(lane*output_bits)
    return result

def edges(bits):
    sign=1<<(bits-1);mask=(1<<bits)-1
    return (0,1,2,sign-2,sign-1,sign,sign+1,sign+2,mask-2,mask-1,mask,0xAAAAAAAAAAAAAAAA&mask,0x5555555555555555&mask)

def corpus(case_type,family=None):
    rng,cases,physical=random.Random(0xAB6020264),[],(0,31,30,3)
    for bits in (8,16,32):
        values,out_values=edges(bits),edges(bits)
        for q in (0,1):
            for operation,op in enumerate(OPS):
                if family is not None and op!=family:continue
                for alias,(rd,rn,rm) in enumerate(ALIASES):
                    for profile in range(185):
                        vectors=[rng.getrandbits(128) for _ in range(4)]
                        if profile<169:
                            vectors[rd]=sum(out_values[(profile//13+lane*5)%13]<<(bits*lane) for lane in range(128//bits))
                            vectors[rn]=sum(values[(profile//13+lane)%13]<<(bits*lane) for lane in range(128//bits))
                            vectors[rm]=sum(values[(profile%13+lane*3)%13]<<(bits*lane) for lane in range(128//bits))
                        if profile==169:
                            vectors[rn]=MASK64
                            vectors[rm]=MASK64<<64
                        seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                        seeds.update({27:0x0800009F if profile&1 else 0x91,28:0x03400000})
                        initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                        word=encode(bits,q,operation,physical[rd],physical[rn],physical[rm])
                        capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                        name=f"absnarrow_{op}_e{bits}_q{q}_a{alias}_p{profile}"
                        cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"absnarrow",bytes(range(64))))
    count=22200 if family is None else 5550
    if len(cases)!=count or len({c.name for c in cases})!=len(cases):raise ValueError("Absolute narrow coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,atext,_=case.name.split("_")
    bits,q=int(width[1:]),int(qtext[1:]);operation=OPS.index(op);rd,rn,rm=ALIASES[int(atext[1:])]
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    vectors[rd]=reference(vectors[rn],vectors[rm],vectors[rd],bits,operation,q)
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
            raise ValueError(f"{case.name}: independent absolute narrow difference/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} absolute narrow difference/signed unsigned magnitude/wrap/status/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--verify',action='store_true',required=True);parser.add_argument('--family',choices=OPS)
    args=parser.parse_args();raise SystemExit(verify(args.family))
