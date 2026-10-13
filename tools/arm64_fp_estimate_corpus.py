"""SIMD FP estimates, independent rational quantization and integer root oracle."""
import argparse
import math
import random
import struct
from fractions import Fraction

OPS=("frecpe","frsqrte","frecpx")
SHAPES=((16,0,0),(16,1,0),(32,0,0),(32,1,0),(64,1,0),(16,1,1),(32,1,1),(64,1,1))
ALIASES=((0,1),(1,1),(2,1))
CONTROLS=(0,1<<22,2<<22,3<<22,1<<19,1<<24,1<<25,(1<<19)|(1<<24)|(1<<25))
MASK64=(1<<64)-1

def encode(bits,q,scalar,operation,rd=0,rn=1):
    bases=(0x0EF9D800,0x2EF9D800,0x5EF9F800) if bits==16 else (0x0EA1D800,0x2EA1D800,0x5EA1F800)
    return bases[operation]|(q<<30)|(scalar<<28)|((bits==64)<<22)|(rn<<5)|rd

def reference(value,bits,operation,fpcr):
    fraction_bits,bias={16:(10,15),32:(23,127),64:(52,1023)}[bits]
    sign=value&(1<<(bits-1));fraction_mask=(1<<fraction_bits)-1
    exponent,fraction=(value>>fraction_bits)&(2*bias+1),value&fraction_mask
    infinity,quiet,flags=(2*bias+1)<<fraction_bits,1<<(fraction_bits-1),0
    if exponent==2*bias+1 and fraction:
        return (infinity|quiet) if fpcr&(1<<25) else value|quiet,0 if fraction&quiet else 1
    if not exponent and fraction and fpcr&(1<<(19 if bits==16 else 24)):
        fraction=0;flags|=0 if bits==16 else 128
    if operation==2: return sign|((2*bias+1-exponent if exponent else 2*bias)<<fraction_bits),flags
    if not exponent and not fraction: return sign|infinity,flags|2
    if operation==1 and sign: return infinity|quiet,flags|1
    if exponent==2*bias+1: return sign,flags
    significand=fraction|(1<<fraction_bits) if exponent else fraction
    if not exponent:
        shift=fraction_bits-(significand.bit_length()-1)
        exponent=1-shift;significand<<=shift
    if operation==0 and exponent < -1:
        mode=(fpcr>>22)&3
        inf=mode==0 or mode==1 and not sign or mode==2 and bool(sign)
        return sign|(infinity if inf else infinity-1),flags|20
    scale=256 if operation==0 else 128
    interval=(significand*scale)//(1<<fraction_bits)
    midpoint=Fraction(2*interval+1,4*scale*(2 if operation==1 and exponent&1 else 1))
    if operation==0: estimate=round(256/midpoint)
    else:
        squared=65536/midpoint
        estimate=math.isqrt(squared.numerator//squared.denominator)
        if Fraction((2*estimate+1)**2,4)<squared: estimate+=1
    if operation==0:
        output_exponent=2*bias-1-exponent
        if output_exponent<=0:
            if fpcr&(1<<(19 if bits==16 else 24)): return sign,flags|8
            return sign|((estimate<<(fraction_bits-8))>>(1-output_exponent)),flags
        return sign|(output_exponent<<fraction_bits)|((estimate-256)<<(fraction_bits-8)),flags
    return ((3*bias-1-exponent)//2)<<fraction_bits|((estimate-256)<<(fraction_bits-8)),flags

def edges(bits):
    fraction,bias={16:(10,15),32:(23,127),64:(52,1023)}[bits]
    sign,inf,quiet,one=1<<(bits-1),(2*bias+1)<<fraction,1<<(fraction-1),bias<<fraction
    threshold,minimum=1<<(fraction-2),1<<fraction
    return (0,sign,one,sign|one,one-1,one+1,1,sign|1,minimum-1,sign|(minimum-1),minimum,sign|minimum,
            threshold-1,threshold,threshold+1,sign|(threshold-1),sign|threshold,sign|(threshold+1),
            inf,sign|inf,inf-1,sign|(inf-1),inf|quiet|3,sign|inf|quiet|5,inf|1,sign|inf|3,
            one+quiet,sign|(one+quiet),(bias+1)<<fraction,(bias-1)<<fraction)

def probe():
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
    import unicorn.arm64_const as r
    count=0
    for bits,q,scalar in SHAPES:
        for operation in range(3):
            if operation==2 and not scalar: continue
            for fpcr in CONTROLS:
                for value in edges(bits):
                    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
                    cpu.mem_map(0x1000,4096);cpu.mem_write(0x1000,struct.pack("<I",encode(bits,q,scalar,operation)))
                    cpu.reg_write(r.UC_ARM64_REG_Q1,value);cpu.reg_write(r.UC_ARM64_REG_Q0,(1<<128)-1)
                    cpu.reg_write(r.UC_ARM64_REG_FPCR,fpcr);cpu.reg_write(r.UC_ARM64_REG_FPSR,0x08000010)
                    cpu.emu_start(0x1000,0x1004,count=1)
                    result,flags=reference(value,bits,operation,fpcr)
                    for lane in range(1,1 if scalar else (128 if q else 64)//bits):
                        extra,exc=reference(0,bits,operation,fpcr);result|=extra<<(lane*bits);flags|=exc
                    got=(cpu.reg_read(r.UC_ARM64_REG_Q0),cpu.reg_read(r.UC_ARM64_REG_FPSR))
                    if got!=(result,0x08000010|flags): raise ValueError((bits,q,scalar,OPS[operation],hex(fpcr),hex(value),got,(result,0x08000010|flags)))
                    count+=1
    print(f"PASS {count} SIMD estimate rational/root/Unicorn MAX probes")

def corpus(case_type,family=None):
    rng,cases,physical=random.Random(0xC03F0264),[],(0,31,30,3)
    for bits,q,scalar in SHAPES:
        fraction,bias={16:(10,15),32:(23,127),64:(52,1023)}[bits]
        values=edges(bits)
        for operation,op in enumerate(OPS):
            if operation==2 and not scalar: continue
            if family is not None and op!=family: continue
            for alias,(rd,rn) in enumerate(ALIASES):
                for control,fpcr in enumerate(CONTROLS):
                    for profile in range(34):
                        vectors=[rng.getrandbits(128) for _ in range(4)]
                        lhs=[values[(profile+lane)%len(values)] for lane in range(128//bits)]
                        if profile==30:
                            active=1 if scalar else (128 if q else 64)//bits
                            lhs=[values[2] if lane<active else values[24] for lane in range(128//bits)]
                        if profile==31: lhs=[rng.getrandbits(bits) for _ in lhs]
                        if profile>=32:
                            lhs=[((bias+(profile&1))<<fraction)|((lane*37+alias*53+control*19)<<(fraction-8)) for lane in range(128//bits)]
                        vectors[rn]=sum(x<<(lane*bits) for lane,x in enumerate(lhs))
                        seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                        seeds.update({27:0x08000010,28:fpcr})
                        initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                        word=encode(bits,q,scalar,operation,physical[rd],physical[rn])
                        capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                        name=f"fpestimate_{op}_e{bits}_q{q}_s{scalar}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"fpestimate",bytes(range(64))))
    count=15504 if family is None else 2448 if family=="frecpx" else 6528
    if len(cases)!=count or len({c.name for c in cases})!=len(cases): raise ValueError("Estimate coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,stext,atext,_,_=case.name.split("_")
    bits,q,scalar=int(width[1:]),int(qtext[1:]),int(stext[1:])
    operation=OPS.index(op);rd,rn=ALIASES[int(atext[1:])]
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    a,result,flags=vectors[rn],0,case.seeds[27]
    for lane in range(1 if scalar else (128 if q else 64)//bits):
        value,exc=reference((a>>(lane*bits))&((1<<bits)-1),bits,operation,case.seeds[28])
        result|=value<<(lane*bits);flags|=exc
    vectors[rd]=result
    registers={f"x{n}":case.seeds.get(n,0x100+n) for n in range(29)}
    registers.update({"x27":flags,"x28":case.seeds[28],"x29":d.MEMORY_BASE,"x30":d.DATA_BASE})
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
            raise ValueError(f"{case.name}: independent SIMD FP estimates/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} SIMD FP estimates/quantization/midpoint/root/NaN/FZ/FZ16/IDC/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser = argparse.ArgumentParser();parser.add_argument("--probe",action="store_true");parser.add_argument("--verify",action="store_true");parser.add_argument("--family",choices=OPS)
    args = parser.parse_args()
    if args.probe: probe()
    elif args.verify: verify(args.family)
    else: parser.error("choose --probe or --verify")
