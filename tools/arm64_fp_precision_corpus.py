"""Integer significand/exponent oracle for SIMD FP precision conversions."""
import argparse
import random
import struct

OPS = ("fcvtl","fcvtn","fcvtxn")
SHAPES = ((16,0,0),(16,1,0),(32,0,0),(32,1,0),(16,0,1),(16,1,1),(32,0,1),(32,1,1),(32,0,2),(32,1,2))
ALIASES = ((0,1),(1,1),(2,1))
CONTROLS = (0,1<<22,2<<22,3<<22,1<<19,1<<24,1<<25,(1<<19)|(1<<24)|(1<<25))
MASK64 = (1<<64)-1

def encode(small,q,operation,rd=0,rn=1):
    return (0x0E217800,0x0E216800,0x2E216800)[operation] | (q<<30) | ((small==32)<<22) | (rn<<5) | rd

def format(bits): return {16:(10,15),32:(23,127),64:(52,1023)}[bits]

def reference(value,source,destination,odd,fpcr):
    sf,sbias = format(source);df,dbias = format(destination)
    ssign,dsign = 1<<(source-1),1<<(destination-1)
    exponent,fraction,negative = (value>>sf)&(2*sbias+1),value&((1<<sf)-1),bool(value&ssign)
    inf,flags,sign = (2*dbias+1)<<df,0,dsign if negative else 0
    if exponent==2*sbias+1:
        if not fraction: return sign|inf,0
        flags = 0 if fraction&(1<<(sf-1)) else 1
        if fpcr&(1<<25): return inf|(1<<(df-1)),flags
        payload = fraction<<(df-sf) if df>=sf else fraction>>(sf-df)
        return sign|inf|payload|(1<<(df-1)),flags
    if not exponent and fraction and source!=16 and fpcr&(1<<24):
        fraction=0;flags|=128
    if not exponent and not fraction: return sign,flags
    significand = fraction|(1<<sf) if exponent else fraction
    power = (exponent if exponent else 1)-sbias-sf
    top = significand.bit_length()-1+power
    # FZ16 is ignored by both directions of half precision conversion.
    if destination!=16 and fpcr&(1<<24) and top<1-dbias: return sign,flags|8
    shift = max(top,1-dbias)-df-power
    if shift>0: result,remainder = divmod(significand,1<<shift);denominator=1<<shift
    else: result,remainder,denominator = significand<<(-shift),0,1
    if remainder:
        flags |= 16
        if top<1-dbias: flags |= 8
        mode = (fpcr>>22)&3
        if odd: result |= 1
        elif mode==0: result += 2*remainder>denominator or 2*remainder==denominator and bool(result&1)
        elif mode==1 and not negative or mode==2 and negative: result += 1
    rounded_top = max(top,1-dbias)
    if result>=1<<(df+1): result >>= 1;rounded_top+=1
    if rounded_top>dbias:
        mode = (fpcr>>22)&3
        infinity = not odd and (mode==0 or mode==1 and not negative or mode==2 and negative)
        return sign|(inf if infinity else inf-1),flags|20
    encoded_exponent = rounded_top+dbias if result>=1<<df else 0
    return sign|(encoded_exponent<<df)|(result&((1<<df)-1)),flags

def edges(source,destination):
    sf,sbias = format(source);df,dbias = format(destination)
    sign,inf,quiet,one = 1<<(source-1),(2*sbias+1)<<sf,1<<(sf-1),sbias<<sf
    values = [0,sign,1,sign|1,(1<<sf)-1,sign|((1<<sf)-1),1<<sf,sign|(1<<sf),
              one,sign|one,one-1,one+1,inf-1,sign|(inf-1),inf,sign|inf,
              inf|quiet|3,sign|inf|quiet|5,inf|1,sign|inf|3,inf|quiet|((1<<sf)-1)]
    if source>destination:
        def power(exponent): return (exponent+sbias)<<sf
        minimum,subnormal,halfsub = power(1-dbias),power(1-dbias-df),power(-dbias-df)
        overflow = power(dbias+1)
        midpoint = one|(1<<(sf-df-1))
        values += [minimum-1,minimum,minimum+1,subnormal,subnormal+1,halfsub-1,halfsub,halfsub+1,
                   midpoint-1,midpoint,midpoint+1,sign|midpoint,overflow-1,overflow,overflow+1,
                   sign|(overflow-1),sign|overflow]
    return tuple(values)

def probe():
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
    import unicorn.arm64_const as r
    count=0
    for small,q,operation in SHAPES:
        source,destination = (small,small*2) if operation==0 else (small*2,small)
        for fpcr in CONTROLS:
            for value in edges(source,destination):
                cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
                cpu.mem_map(0x1000,4096);cpu.mem_write(0x1000,struct.pack("<I",encode(small,q,operation)))
                source_vector = value<<(64 if operation==0 and q else 0)
                initial = 0xFEDCBA98765432100123456789ABCDEF
                cpu.reg_write(r.UC_ARM64_REG_Q1,source_vector);cpu.reg_write(r.UC_ARM64_REG_Q0,initial)
                cpu.reg_write(r.UC_ARM64_REG_FPCR,fpcr);cpu.reg_write(r.UC_ARM64_REG_FPSR,0x08000002)
                cpu.emu_start(0x1000,0x1004,count=1)
                result,flags=reference(value,source,destination,operation==2,fpcr)
                if operation!=0 and q: result=(result<<64)|(initial&MASK64)
                got=(cpu.reg_read(r.UC_ARM64_REG_Q0),cpu.reg_read(r.UC_ARM64_REG_FPSR))
                if got!=(result,0x08000002|flags): raise ValueError((small,q,OPS[operation],hex(fpcr),hex(value),got,(result,0x08000002|flags)))
                count+=1
    print(f"PASS {count} MAX SIMD precision conversion probes")

def corpus(case_type,family=None):
    rng,cases,physical=random.Random(0xC02F0264),[],(0,31,30,3)
    for small,q,operation in SHAPES:
        op=OPS[operation]
        if family is not None and op!=family: continue
        source,destination=(small,small*2) if operation==0 else (small*2,small)
        values=edges(source,destination)
        for alias,(rd,rn) in enumerate(ALIASES):
            for control,fpcr in enumerate(CONTROLS):
                for profile in range(40):
                    vectors=[rng.getrandbits(128) for _ in range(4)]
                    lhs=[values[(profile+lane)%len(values)] for lane in range(128//source)]
                    if profile==38 and operation==0:
                        lhs=[values[18] if (lane>=64//source)!=bool(q) else values[8] for lane in range(128//source)]
                    if profile==39: lhs=[rng.getrandbits(source) for _ in lhs]
                    vectors[rn]=sum(x<<(lane*source) for lane,x in enumerate(lhs))
                    seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                    seeds.update({27:0x08000002,28:fpcr})
                    initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                    word=encode(small,q,operation,physical[rd],physical[rn])
                    capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                    name=f"fpprecision_{op}_e{small}_q{q}_a{alias}_c{control}_p{profile}"
                    cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"fpprecision",bytes(range(64))))
    count=9600 if family is None else (1920 if family=="fcvtxn" else 3840)
    if len(cases)!=count or len({c.name for c in cases})!=len(cases): raise ValueError("Precision coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,atext,_,_=case.name.split("_")
    small,q,operation=int(width[1:]),int(qtext[1:]),OPS.index(op)
    source,destination=(small,small*2) if operation==0 else (small*2,small)
    rd,rn=ALIASES[int(atext[1:])]
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    a,result,flags=vectors[rn],vectors[rd]&MASK64 if operation!=0 and q else 0,case.seeds[27]
    source_offset=64 if operation==0 and q else 0
    destination_offset=64 if operation!=0 and q else 0
    for lane in range(64//small):
        value,exc=reference((a>>(source_offset+lane*source))&((1<<source)-1),source,destination,operation==2,case.seeds[28])
        result |= value<<(destination_offset+lane*destination);flags |= exc
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
        mnemonic = op + ("2" if case.name.split("_")[3] == "q1" else "")
        if sum(instruction.mnemonic == mnemonic for instruction in decoded) != 1:
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
            raise ValueError(f"{case.name}: independent SIMD precision conversion/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} SIMD precision conversion/rounding/odd/NaN/FZ/FZ16/IDC/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser = argparse.ArgumentParser();parser.add_argument("--probe",action="store_true");parser.add_argument("--verify",action="store_true");parser.add_argument("--family",choices=OPS)
    args = parser.parse_args()
    if args.probe: probe()
    elif args.verify: verify(args.family)
    else: parser.error("choose --probe or --verify")
