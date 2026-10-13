"""Exact integer/rational FPRoundIntN oracle for S/D scalar and vector forms."""
import argparse
import random
import struct

OPS=("frint32z","frint32x","frint64z","frint64x")
BASES=(0x0E21E800,0x2E21E800,0x0E21F800,0x2E21F800)
SCALAR_BASES=(0x1E284000,0x1E28C000,0x1E294000,0x1E29C000)
SHAPES=((32,0,0),(32,1,0),(64,1,0),(32,0,1),(64,0,1))
ALIASES=((0,1),(1,1),(2,1))
CONTROLS=(0,1<<22,2<<22,3<<22,1<<24,1<<25,1<<19,(1<<24)|(1<<25))
MASK64=(1<<64)-1

def encode(bits,q,scalar,operation,rd=0,rn=1):
    return (SCALAR_BASES if scalar else BASES)[operation]|((bits==64)<<22)|(0 if scalar else q<<30)|(rn<<5)|rd

def reference(value,bits,operation,fpcr):
    fraction,bias=(23,127) if bits==32 else (52,1023)
    sign=1<<(bits-1);negative=bool(value&sign)
    exponent,frac=(value>>fraction)&(2*bias+1),value&((1<<fraction)-1)
    integer_bits=32 if operation<2 else 64
    indefinite=sign|((bias+integer_bits-1)<<fraction)
    if exponent==2*bias+1:return indefinite,1
    flags=0
    if exponent==0 and frac and fpcr&(1<<24):frac=0;flags=128
    magnitude=frac|(1<<fraction) if exponent else frac
    shift=(exponent if exponent else 1)-bias-fraction
    if shift>=0:quotient,remainder,denominator=magnitude<<shift,0,1
    else:denominator=1<<(-shift);quotient,remainder=divmod(magnitude,denominator)
    mode=(fpcr>>22)&3 if operation&1 else 3
    increment=bool(remainder) and (mode==1 and not negative or mode==2 and negative)
    if mode==0:increment=2*remainder>denominator or 2*remainder==denominator and quotient&1
    quotient+=int(bool(increment))
    integer=-quotient if negative else quotient
    if integer<-(1<<(integer_bits-1)) or integer>(1<<(integer_bits-1))-1:return indefinite,flags|1
    if remainder:flags|=16
    if not quotient:return sign if negative else 0,flags
    top=quotient.bit_length()-1
    significand=(quotient<<fraction)>>top
    return (sign if negative else 0)|((top+bias)<<fraction)|(significand&((1<<fraction)-1)),flags

def edges(bits,operation):
    fraction,bias=(23,127) if bits==32 else (52,1023)
    sign=1<<(bits-1);inf=(2*bias+1)<<fraction;quiet=1<<(fraction-1)
    one=bias<<fraction;half=(bias-1)<<fraction;minimum=1<<fraction
    bound=(bias+(31 if operation<2 else 63))<<fraction
    values=[0,sign,half,sign|half,half-1,half+1,sign|(half-1),sign|(half+1),one,sign|one,
            one|quiet,sign|one|quiet,one-1,one+1,1,sign|1,minimum-1,sign|(minimum-1),minimum,sign|minimum,
            inf,sign|inf,inf|quiet|3,sign|inf|quiet|5,inf|1,sign|inf|3,inf-1,sign|(inf-1)]
    for delta in (-2,-1,0,1,2):values.extend((bound+delta,sign|(bound+delta)))
    # D/32 can express fractional values immediately beside the integer bound.
    if bits==64 and operation<2:
        step=1<<(fraction-31)
        values.extend((bound-step,bound-step+1,bound-step//2,sign|(bound+step//2),sign|(bound+step//2+1)))
    return values

def probe():
    from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
    import unicorn.arm64_const as r
    decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM);count=0
    for bits,q,scalar in SHAPES:
        for operation in range(4):
            word=encode(bits,q,scalar,operation)
            decoded=list(decoder.disasm(struct.pack("<I",word),0))
            if len(decoded)!=1 or decoded[0].mnemonic!=OPS[operation]:raise ValueError(hex(word))
            for fpcr in CONTROLS:
                for value in edges(bits,operation):
                    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
                    cpu.mem_map(0x1000,4096);cpu.mem_write(0x1000,struct.pack("<I",word))
                    cpu.reg_write(r.UC_ARM64_REG_Q1,value);cpu.reg_write(r.UC_ARM64_REG_Q0,(1<<128)-1)
                    cpu.reg_write(r.UC_ARM64_REG_FPCR,fpcr);cpu.reg_write(r.UC_ARM64_REG_FPSR,0x08000002)
                    cpu.emu_start(0x1000,0x1004,count=1)
                    result,flags=reference(value,bits,operation,fpcr)
                    got=cpu.reg_read(r.UC_ARM64_REG_Q0),cpu.reg_read(r.UC_ARM64_REG_FPSR)
                    if got!=(result,0x08000002|flags):raise ValueError((bits,q,scalar,OPS[operation],hex(fpcr),hex(value),got,(result,0x08000002|flags)))
                    count+=1
    print(f"PASS {count} FRINT range boundary/NaN/rounding/IOC/IXC/Unicorn probes")

def corpus(case_type,family=None):
    rng,cases,physical=random.Random(0xF032640126),[],(0,31,30,3)
    for bits,q,scalar in SHAPES:
        for operation,op in enumerate(OPS):
            if family is not None and op!=family:continue
            values=edges(bits,operation)
            for alias,(rd,rn) in enumerate(ALIASES):
                for control,fpcr in enumerate(CONTROLS):
                    for profile in range(48):
                        vectors=[rng.getrandbits(128) for _ in range(4)]
                        lhs=[values[(profile+lane)%len(values)] if profile<46 else rng.getrandbits(bits) for lane in range(128//bits)]
                        if profile==46:
                            active=1 if scalar else (128 if q else 64)//bits
                            lhs=[0 if lane<active else values[24] for lane in range(128//bits)]
                        vectors[rn]=sum(x<<(lane*bits) for lane,x in enumerate(lhs))
                        seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                        seeds.update({27:0x08000002,28:fpcr})
                        initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                        word=encode(bits,q,scalar,operation,physical[rd],physical[rn])
                        capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                        name=f"frintrange_{op}_e{bits}_q{q}_s{scalar}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"frintrange",bytes(range(64))))
    count=23040 if family is None else 5760
    if len(cases)!=count or len({c.name for c in cases})!=len(cases):raise ValueError("FRINT range coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,stext,atext,_,_ = case.name.split("_")
    bits,q,scalar = int(width[1:]),int(qtext[1:]),int(stext[1:])
    rd,rn = ALIASES[int(atext[1:])];operation = OPS.index(op)
    vectors = [case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    a,result,flags = vectors[rn],0,case.seeds[27]
    for lane in range(1 if scalar else (128 if q else 64)//bits):
        value,exc = reference((a>>(lane*bits))&((1<<bits)-1),bits,operation,case.seeds[28])
        result |= value<<(lane*bits);flags |= exc
    vectors[rd] = result
    registers = {f"x{n}":case.seeds.get(n,0x100+n) for n in range(29)}
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
            raise ValueError(f"{case.name}: independent FRINT range/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} FRINT range/rounding/indefinite/NaN/FZ/IDC/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser = argparse.ArgumentParser();parser.add_argument("--probe",action="store_true");parser.add_argument("--verify",action="store_true");parser.add_argument("--family",choices=OPS)
    args = parser.parse_args()
    if args.probe: probe()
    elif args.verify: verify(args.family)
    else: parser.error("choose --probe or --verify")
