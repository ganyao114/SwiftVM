"""SIMD pairwise and across-lane FP reduction exact integer oracle."""
import argparse
import random
import struct
PAIR_OPS=("faddp","fmaxp","fminp","fmaxnmp","fminnmp")
ACROSS_OPS=("fmaxv","fminv","fmaxnmv","fminnmv")
SHAPES=((16,0),(16,1),(32,0),(32,1),(64,1))
ALIASES=((0,1,2),(1,1,2),(2,1,2),(1,1,1),(3,1,2))
CONTROLS=(0,1<<22,2<<22,3<<22,1<<19,1<<24,1<<25,(1<<19)|(1<<24)|(1<<25))
MASK64=(1<<64)-1

def encode(bits,q,across,operation,rd=0,rn=1,rm=2):
    if across:
        bases=(0x0E30F800,0x0EB0F800,0x0E30C800,0x0EB0C800) if bits==16 else (0x2E30F800,0x2EB0F800,0x2E30C800,0x2EB0C800)
    else:
        bases=(0x2E401400,0x2E403400,0x2EC03400,0x2E400400,0x2EC00400) if bits==16 else (0x2E20D400,0x2E20F400,0x2EA0F400,0x2E20C400,0x2EA0C400)
    return bases[operation]|(q<<30)|((bits==64)<<22)|(rn<<5)|rd|(0 if across else rm<<16)

def reduce_reference(values,bits,op,fpcr,tree="architectural"):
    # Architectural ordered reduction uses adjacent half trees, confirmed
    # against Unicorn with distinct signaling NaN payloads.
    pair_op={"fmaxv":"fmaxp","fminv":"fminp","fmaxnmv":"fmaxnmp","fminnmv":"fminnmp"}[op]
    flags=0
    def node(a,b):
        nonlocal flags
        result,exc=reference(a,b,bits,pair_op,fpcr);flags|=exc
        return result
    if tree=="flat":
        result=values[0]
        for value in values[1:]: result=node(result,value)
        return result,flags
    pairs=[node(values[i],values[i+1]) for i in range(0,len(values),2)]
    if len(pairs)==4:
        if tree=="interleave": return node(node(pairs[0],pairs[2]),node(pairs[1],pairs[3])),flags
        return node(node(pairs[0],pairs[1]),node(pairs[2],pairs[3])),flags
    return node(pairs[0],pairs[1]),flags

def tree_probe():
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
    import unicorn.arm64_const as r
    count,distinguished_flat,distinguished_adjacent=0,0,0
    for bits,q in ((16,0),(16,1),(32,1)):
        fraction,bias={16:(10,15),32:(23,127)}[bits]
        one,inf,quiet=bias<<fraction,(2*bias+1)<<fraction,1<<(fraction-1)
        count_lanes=(128 if q else 64)//bits
        for operation,op in enumerate(ACROSS_OPS):
            for first in range(count_lanes):
                for second in range(count_lanes):
                    for profile in range(4):
                        values=[one+i for i in range(count_lanes)]
                        values[first]=inf|(quiet if profile&1 else 0)|3
                        values[second]=inf|(quiet if profile&2 else 0)|5
                        vector=sum(value<<(lane*bits) for lane,value in enumerate(values))
                        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
                        cpu.mem_map(0x1000,4096);cpu.mem_write(0x1000,struct.pack("<I",encode(bits,q,1,operation)))
                        cpu.reg_write(r.UC_ARM64_REG_Q1,vector);cpu.reg_write(r.UC_ARM64_REG_FPSR,0)
                        cpu.emu_start(0x1000,0x1004,count=1)
                        got=(cpu.reg_read(r.UC_ARM64_REG_Q0),cpu.reg_read(r.UC_ARM64_REG_FPSR))
                        expected=reduce_reference(values,bits,op,0)
                        if got!=expected: raise ValueError((bits,q,op,first,second,profile,got,expected))
                        distinguished_flat+=got!=reduce_reference(values,bits,op,0,"flat")
                        distinguished_adjacent+=got!=reduce_reference(values,bits,op,0,"interleave")
                        count+=1
    if not distinguished_flat or not distinguished_adjacent: raise ValueError("Probe failed to distinguish reduction trees")
    print(f"PASS {count} directed Unicorn reduction tree/NaN priority probes; differs from flat={distinguished_flat}, VIXL interleave={distinguished_adjacent}")
def reference(a, b, bits, op, fpcr):
    fraction = {16: 10, 32: 23, 64: 52}[bits]
    bias = {16: 15, 32: 127, 64: 1023}[bits]
    maximum_exponent, sign = 2 * bias + 1, 1 << (bits - 1)
    fraction_mask = (1 << fraction) - 1
    exponent_mask, quiet = maximum_exponent << fraction, 1 << (fraction - 1)
    flush, flags = bool(fpcr & (1 << (19 if bits == 16 else 24))), 0

    def nan(value):
        return value & exponent_mask == exponent_mask and bool(value & fraction_mask)

    def signaling(value):
        return nan(value) and not value & quiet

    if flush:
        if not a & exponent_mask and a & fraction_mask:
            a &= sign
            if bits != 16:
                flags |= 128
        if not b & exponent_mask and b & fraction_mask:
            b &= sign
            if bits != 16:
                flags |= 128
    numeric, minimum = op in ("fmaxnmp", "fminnmp"), op in ("fminp", "fminnmp")
    snan = signaling(a) or signaling(b)
    if snan or ((nan(a) or nan(b)) and (op == "faddp" or not numeric or (nan(a) and nan(b)))):
        if snan:
            flags |= 1
        selected = a if signaling(a) else b if signaling(b) else a if nan(a) else b
        return (exponent_mask | quiet if fpcr & (1 << 25) else selected | quiet), flags
    if nan(a):
        return b, flags
    if nan(b):
        return a, flags
    if op != "faddp":
        if not a & ~sign and not b & ~sign:
            return a | b if minimum else a & b, flags
        less = bool(a & sign) if bool(a & sign) != bool(b & sign) else (a > b if a & sign else a < b)
        return a if minimum == less else b, flags
    ainf, binf = a & ~sign == exponent_mask, b & ~sign == exponent_mask
    if ainf or binf:
        if ainf and binf and bool(a & sign) != bool(b & sign):
            return exponent_mask | quiet, flags | 1
        return a if ainf else b, flags

    def magnitude(value):
        exponent, significand = (value & exponent_mask) >> fraction, value & fraction_mask
        return ((significand | (1 << fraction)) << (exponent - 1)) if exponent else significand

    exact = (-magnitude(a) if a & sign else magnitude(a)) + (-magnitude(b) if b & sign else magnitude(b))
    rounding = (fpcr >> 22) & 3
    if not exact:
        zero_sign = a & sign if bool(a & sign) == bool(b & sign) else sign if rounding == 2 else 0
        return zero_sign, flags
    negative, exact = exact < 0, abs(exact)
    result_sign = sign if negative else 0
    if flush and exact < 1 << fraction:
        return result_sign, flags | 8
    shift = max(0, exact.bit_length() - 1 - fraction)
    rounded, remainder = exact >> shift, exact - ((exact >> shift) << shift)
    if remainder:
        flags |= 16
        increment = not negative if rounding == 1 else negative if rounding == 2 else False
        if rounding == 0:
            half = 1 << (shift - 1)
            increment = remainder > half or (remainder == half and bool(rounded & 1))
        rounded += increment
    if rounded >= 1 << (fraction + 1):
        rounded >>= 1
        shift += 1
    exponent = 0 if rounded < 1 << fraction else shift + 1
    if exponent >= maximum_exponent:
        infinity = rounding == 0 or (rounding == 1 and not negative) or (rounding == 2 and negative)
        return result_sign | (exponent_mask if infinity else exponent_mask - 1), flags | 20
    return result_sign | (exponent << fraction) | (rounded & fraction_mask), flags

def edges(bits):
    fraction,bias={16:(10,15),32:(23,127),64:(52,1023)}[bits]
    sign,inf,quiet,one=1<<(bits-1),(2*bias+1)<<fraction,1<<(fraction-1),bias<<fraction
    return (0,sign,one,sign|one,one-1,one+1,1,sign|1,(1<<fraction)-1,sign|((1<<fraction)-1),1<<fraction,sign|(1<<fraction),
            inf,sign|inf,inf-1,sign|(inf-1),inf|quiet|3,sign|inf|quiet|5,inf|1,sign|inf|3,
            (bias-fraction-1)<<fraction,sign|((bias-fraction-1)<<fraction),one|quiet,sign|one|quiet)

def corpus(case_type,family=None):
    rng,cases,physical=random.Random(0xC04F0264),[],(0,31,30,3)
    for bits,q in SHAPES:
        values=edges(bits)
        fraction,bias={16:(10,15),32:(23,127),64:(52,1023)}[bits]
        inf,quiet,one=(2*bias+1)<<fraction,1<<(fraction-1),bias<<fraction
        active=(128 if q else 64)//bits
        for across in (0,1):
            if across and (bits==64 or bits==32 and not q): continue
            for operation,op in enumerate(ACROSS_OPS if across else PAIR_OPS):
                if family is not None and op!=family: continue
                for alias,(rd,rn,rm) in enumerate(ALIASES):
                    for control,fpcr in enumerate(CONTROLS):
                        for profile in range(32):
                            vectors=[rng.getrandbits(128) for _ in range(4)]
                            lhs=[values[(profile+lane)%len(values)] for lane in range(128//bits)]
                            rhs=[values[(profile*3+lane+alias)%len(values)] for lane in range(128//bits)]
                            if profile==24:
                                lhs=[one if lane<active else inf|1 for lane in range(128//bits)]
                                rhs=[one if lane<active else 1 for lane in range(128//bits)]
                            if profile==25:
                                lhs=[rng.getrandbits(bits) for _ in lhs];rhs=[rng.getrandbits(bits) for _ in rhs]
                            if profile>=26:
                                lhs=[one+lane for lane in range(128//bits)];rhs=list(reversed(lhs))
                                first,second=((2,4),(0,2),(1,3),(0,1),(2,3),(0,active-1))[profile-26]
                                lhs[first%active]=inf|(quiet if profile&1 else 0)|3
                                lhs[second%active]=inf|(quiet if profile&2 else 0)|5
                                rhs[first%active]=inf|quiet|7;rhs[second%active]=inf|9
                            vectors[rn]=sum(x<<(lane*bits) for lane,x in enumerate(lhs))
                            if not across and rn!=rm: vectors[rm]=sum(x<<(lane*bits) for lane,x in enumerate(rhs))
                            seeds={1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                            seeds.update({27:0x08000010,28:fpcr})
                            initialize=tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                            word=encode(bits,q,across,operation,physical[rd],physical[rn],physical[rm])
                            capture=(0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                            name=f"fpreduction_{op}_e{bits}_q{q}_a{alias}_c{control}_p{profile}"
                            cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"fpreduction",bytes(range(64))))
    count=47360 if family is None else 6400 if family in PAIR_OPS else 3840
    if len(cases)!=count or len({c.name for c in cases})!=len(cases): raise ValueError("Reduction coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,atext,_,_=case.name.split("_")
    bits,q=int(width[1:]),int(qtext[1:]);across=op in ACROSS_OPS
    rd,rn,rm=ALIASES[int(atext[1:])]
    vectors=[case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    a,b,result,flags=vectors[rn],vectors[rm],0,case.seeds[27]
    active=(128 if q else 64)//bits
    values=lambda v: [(v>>(lane*bits))&((1<<bits)-1) for lane in range(active)]
    if across:
        result,exc=reduce_reference(values(a),bits,op,case.seeds[28]);flags|=exc
    else:
        for lane in range(active):
            source=values(a if lane<active//2 else b);index=(lane%(active//2))*2
            value,exc=reference(source[index],source[index+1],bits,op,case.seeds[28])
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
            raise ValueError(f"{case.name}: independent SIMD FP reductions/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} SIMD FP reductions/rounding/tree/NaN/FZ/FZ16/IDC/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser = argparse.ArgumentParser();parser.add_argument("--probe",action="store_true");parser.add_argument("--verify",action="store_true");parser.add_argument("--family",choices=PAIR_OPS+ACROSS_OPS)
    args = parser.parse_args()
    if args.probe: tree_probe()
    elif args.verify: verify(args.family)
    else: parser.error("choose --probe or --verify")
