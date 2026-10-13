"""Exact scaled-rational oracle for SIMD fixed-point integer conversions."""
import argparse
import random
import struct

OPS = ("scvtf","ucvtf","fcvtzs","fcvtzu")
BASES = (0x0F00E400,0x2F00E400,0x0F00FC00,0x2F00FC00)
SHAPES = ((16,0,0),(16,1,0),(32,0,0),(32,1,0),(64,1,0),(16,1,1),(32,1,1),(64,1,1))
ALIASES = ((0,1),(1,1),(2,1))
CONTROLS = (0,1<<22,2<<22,3<<22,1<<19,1<<24,1<<25,(1<<19)|(1<<24)|(1<<25))
MASK64 = (1<<64)-1

def encode(bits,q,scalar,operation,fbits,rd=0,rn=1):
    return BASES[operation]|(q<<30)|(scalar<<28)|((2*bits-fbits)<<16)|(rn<<5)|rd

def rounded(quotient,remainder,denominator,negative,mode):
    if not remainder: return quotient
    increment = mode==1 and not negative or mode==2 and negative
    if mode in (0,4): increment = 2*remainder>denominator or 2*remainder==denominator and (mode==4 or quotient&1)
    return quotient+int(bool(increment))

def reference(value,bits,operation,fpcr,fbits):
    fraction,bias = {16:(10,15),32:(23,127),64:(52,1023)}[bits]
    sign,mask = 1<<(bits-1),(1<<bits)-1
    inf,flags = (2*bias+1)<<fraction,0
    unsigned = bool(operation&1)
    if operation<2:
        negative = not unsigned and bool(value&sign)
        integer = value-(1<<bits) if negative else value
        magnitude = abs(integer)
        if not magnitude: return 0,0
        top = magnitude.bit_length()-1-fbits
        if fpcr&(1<<(19 if bits==16 else 24)) and top<1-bias:
            return sign if negative else 0,8
        shift = max(top,1-bias)-fraction+fbits
        if shift>0: significand,remainder,denominator = magnitude>>shift,magnitude&((1<<shift)-1),1<<shift
        else: significand,remainder,denominator = magnitude<<(-shift),0,1
        mode = (fpcr>>22)&3
        significand = rounded(significand,remainder,denominator,negative,mode)
        if remainder:
            flags |= 16
            if top<1-bias: flags |= 8
        top = max(top,1-bias)
        if significand>=1<<(fraction+1): significand >>= 1;top += 1
        if top>bias:
            flags |= 20
            infinity = mode==0 or mode==1 and not negative or mode==2 and negative
            return (sign if negative else 0) | (inf if infinity else inf-1),flags
        encoded_exponent = top+bias if significand>=1<<fraction else 0
        return (sign if negative else 0)|(encoded_exponent<<fraction)|(significand&((1<<fraction)-1)),flags
    negative = bool(value&sign)
    exponent,frac = (value>>fraction)&(2*bias+1),value&((1<<fraction)-1)
    if exponent==2*bias+1:
        if frac: return 0,1
        low,high = (0,mask) if unsigned else (-sign,sign-1)
        return (low if negative else high)&mask,1
    if exponent==0 and frac and fpcr&(1<<(19 if bits==16 else 24)):
        frac = 0;flags |= 0 if bits==16 else 128
    magnitude = frac|(1<<fraction) if exponent else frac
    shift = (exponent if exponent else 1)-bias-fraction+fbits
    if shift>=0: quotient,remainder,denominator = magnitude<<shift,0,1
    else: denominator = 1<<(-shift);quotient,remainder = divmod(magnitude,denominator)
    mode = 3
    quotient = rounded(quotient,remainder,denominator,negative,mode)
    integer = -quotient if negative else quotient
    low,high = (0,mask) if unsigned else (-sign,sign-1)
    if integer<low or integer>high: return min(high,max(low,integer))&mask,flags|1
    return integer&mask,flags|(16 if remainder else 0)

def edges(bits,operation,fbits):
    fraction,bias = {16:(10,15),32:(23,127),64:(52,1023)}[bits]
    sign,mask = 1<<(bits-1),(1<<bits)-1
    if operation<2:
        pivot = 1<<(fraction+1)
        return (0,1,2,mask,sign,sign-1,sign+1,pivot-1,pivot,pivot+1,mask-1,
                (-pivot-1)&mask,(-pivot+1)&mask,2049,(-2049)&mask,65519&mask,65520&mask,65535&mask)
    inf,quiet,one,half = (2*bias+1)<<fraction,1<<(fraction-1),bias<<fraction,(bias-1)<<fraction
    bound = (bias+bits-1-fbits)<<fraction
    ubound = (bias+bits-fbits)<<fraction
    return (0,sign,half,sign|half,one|quiet,sign|one|quiet,one+(1<<(fraction-2)),sign|(one+(1<<(fraction-2))),
            (bias+1)<<fraction|1<<(fraction-2),sign|((bias+1)<<fraction|1<<(fraction-2)),
            1,sign|1,(1<<fraction)-1,sign|((1<<fraction)-1),1<<fraction,sign|(1<<fraction),
            inf,sign|inf,inf|quiet|3,sign|inf|quiet|5,inf|1,sign|inf|3,
            bound-1,bound,bound+1,sign|bound,sign|(bound+1),ubound-1,ubound,inf-1,sign|(inf-1))

def unicorn_result(value,bits,scalar,operation):
    # Unicorn 2.1.4 scalar fixed FCVTZS H writes its signed helper's i32
    # result with write_fp_sreg. ARM writes only the H element. Accept exactly
    # that documented sign extension, never mask arbitrary emulator output.
    return value | 0xFFFF0000 if bits==16 and scalar and operation==2 and value&0x8000 else value

def probe():
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
    import unicorn.arm64_const as r
    count=known_bug=0
    for bits,q,scalar in SHAPES:
        for operation in range(4):
            for fbits in range(1,bits+1):
                values=edges(bits,operation,fbits)
                indices=(1,3,4,9,10,17) if operation<2 else (3,4,10,18,24,28)
                for fpcr in CONTROLS:
                    for index in indices:
                        value=values[index]
                        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
                        cpu.mem_map(0x1000,4096);cpu.mem_write(0x1000,struct.pack("<I",encode(bits,q,scalar,operation,fbits)))
                        cpu.reg_write(r.UC_ARM64_REG_Q1,value);cpu.reg_write(r.UC_ARM64_REG_Q0,(1<<128)-1)
                        cpu.reg_write(r.UC_ARM64_REG_FPCR,fpcr);cpu.reg_write(r.UC_ARM64_REG_FPSR,0x08000002)
                        cpu.emu_start(0x1000,0x1004,count=1)
                        expected,flags=reference(value,bits,operation,fpcr,fbits)
                        got=(cpu.reg_read(r.UC_ARM64_REG_Q0),cpu.reg_read(r.UC_ARM64_REG_FPSR))
                        oracle=unicorn_result(expected,bits,scalar,operation)
                        if got!=(oracle,0x08000002|flags): raise ValueError((bits,q,scalar,OPS[operation],fbits,hex(fpcr),hex(value),got,(oracle,0x08000002|flags)))
                        known_bug+=oracle!=expected
                        count+=1
    print(f"PASS {count} SIMD fixed integer probes; {known_bug} explicitly checked Unicorn scalar H FCVTZS sign-extension errata")

def corpus(case_type,family=None,include_known_bug=False):
    rng,cases,physical=random.Random(0xC05F0264),[],(0,31,30,3)
    for bits,q,scalar in SHAPES:
        for operation,op in enumerate(OPS):
            if family is not None and op!=family: continue
            if not include_known_bug and bits==16 and scalar and operation==2: continue
            for bucket in (0,1):
                scales=range(1,bits+1) if bucket==0 else (1,bits//2,bits)
                aliases=(0,) if bucket==0 else range(3)
                controls=(0,) if bucket==0 else range(8)
                for fbits in scales:
                    values=edges(bits,operation,fbits)
                    for alias in aliases:
                        rd,rn=ALIASES[alias]
                        for control in controls:
                            fpcr=CONTROLS[control]
                            for profile in range(2 if bucket==0 else 16):
                                vectors=[rng.getrandbits(128) for _ in range(4)]
                                if bucket==0:
                                    lhs=[rng.getrandbits(bits) if profile else values[4 if operation<2 else 24] for _ in range(128//bits)]
                                else:
                                    indices=(0,1,3,4,5,7,9,10,11,13,15,16,17,2,6,8) if operation<2 else (0,3,4,10,11,16,17,18,19,20,21,22,23,24,26,28)
                                    lhs=[values[(indices[profile]+lane)%len(values)] for lane in range(128//bits)]
                                    if profile==14:
                                        active=1 if scalar else (128 if q else 64)//bits
                                        lhs=[0 if lane<active else values[-1] for lane in range(128//bits)]
                                    if profile==15: lhs=[rng.getrandbits(bits) for _ in lhs]
                                vectors[rn]=sum(x<<(lane*bits) for lane,x in enumerate(lhs))
                                seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                                seeds.update({27:0x08000002,28:fpcr})
                                initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                                word=encode(bits,q,scalar,operation,fbits,physical[rd],physical[rn])
                                capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                                name=f"fpfixedsimd_{op}_e{bits}_q{q}_s{scalar}_f{fbits}_b{bucket}_a{alias}_c{control}_p{profile}"
                                cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"fpfixed",bytes(range(64))))
    count=39040 if family is None else 9760
    if not include_known_bug and family in (None,"fcvtzs"): count-=1184
    if len(cases)!=count or len({c.name for c in cases})!=len(cases): raise ValueError("Fixed conversion coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,stext,ftext,_,atext,_,_=case.name.split("_")
    bits,q,scalar,fbits=int(width[1:]),int(qtext[1:]),int(stext[1:]),int(ftext[1:])
    operation=OPS.index(op);rd,rn=ALIASES[int(atext[1:])]
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    a,result,flags=vectors[rn],0,case.seeds[27]
    for lane in range(1 if scalar else (128 if q else 64)//bits):
        value,exc=reference((a>>(lane*bits))&((1<<bits)-1),bits,operation,case.seeds[28],fbits)
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
    cases = corpus(differential.Case,family,include_known_bug=True)
    known_bug=0
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
        _,op,width,_,stext,_,_,atext,_,_=case.name.split("_")
        rd=ALIASES[int(atext[1:])][0];start=16*rd
        oracle=bytearray(wanted["memory"])
        canonical=int.from_bytes(oracle[start:start+16],"little")
        raw=unicorn_result(canonical,int(width[1:]),int(stext[1:]),OPS.index(op))
        oracle[start:start+16]=raw.to_bytes(16,"little");known_bug+=raw!=canonical
        if bytes.fromhex(state["memory"]) != oracle or int(state["nzcv"], 16) != wanted["nzcv"]:
            raise ValueError(f"{case.name}: independent SIMD fixed integer conversion/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} SIMD fixed integer/full ELF/Capstone cases; {known_bug} explicitly checked Unicorn scalar H FCVTZS sign-extension errata")
    return 0


if __name__=="__main__":
    parser = argparse.ArgumentParser();parser.add_argument("--probe",action="store_true");parser.add_argument("--verify",action="store_true");parser.add_argument("--family",choices=OPS)
    args = parser.parse_args()
    if args.probe: probe()
    elif args.verify: verify(args.family)
    else: parser.error("choose --probe or --verify")
