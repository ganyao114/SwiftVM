"""SIMD FP comparison oracle using integer encodings, with full ELF snapshots."""
import argparse
import random
import struct

REG_OPS = ("fcmeq", "fcmge", "fcmgt", "facge", "facgt")
ZERO_OPS = ("fcmeq", "fcmge", "fcmgt", "fcmle", "fcmlt")
REG_BASES = (0x0E20E400,0x2E20E400,0x2EA0E400,0x2E20EC00,0x2EA0EC00)
H_REG_BASES = (0x0E402400,0x2E402400,0x2EC02400,0x2E402C00,0x2EC02C00)
ZERO_BASES = (0x0EA0D800,0x2EA0C800,0x0EA0C800,0x2EA0D800,0x0EA0E800)
H_ZERO_BASES = (0x0EF8D800,0x2EF8C800,0x0EF8C800,0x2EF8D800,0x0EF8E800)
SHAPES = ((16,0,0),(16,1,0),(32,0,0),(32,1,0),(64,1,0),(16,1,1),(32,1,1),(64,1,1))
ALIASES = ((0,1,2),(1,1,2),(2,1,2),(1,1,1),(3,1,2))
CONTROLS = (0,1<<19,1<<24,1<<25,(1<<19)|(1<<24)|(1<<25),1<<22,2<<22,3<<22)
MASK64 = (1<<64)-1

def encode(bits,q,scalar,zero,operation,rd=0,rn=1,rm=2):
    bases = (H_ZERO_BASES if zero else H_REG_BASES) if bits==16 else (ZERO_BASES if zero else REG_BASES)
    return bases[operation] | (q<<30) | (scalar<<28) | ((bits==64)<<22) | (rn<<5) | rd | (0 if zero else rm<<16)

def reference(a,b,bits,op,fpcr):
    fraction = {16:10,32:23,64:52}[bits]
    sign,mag = 1<<(bits-1),(1<<(bits-1))-1
    exponent = mag ^ ((1<<fraction)-1)
    quiet = 1<<(fraction-1)
    flags = 0
    def unpack(x):
        nonlocal flags
        if (x & exponent)==0 and x & ((1<<fraction)-1) and fpcr & (1<<(19 if bits==16 else 24)):
            flags |= 0 if bits==16 else 128
            x &= sign
        return x
    a,b = unpack(a),unpack(b)
    nan = lambda x: (x & exponent)==exponent and bool(x & ((1<<fraction)-1))
    if nan(a) or nan(b):
        invalid = op!="fcmeq" or any(nan(x) and not x&quiet for x in (a,b))
        return 0,flags | int(invalid)
    if op.startswith("fac"):
        a &= mag;b &= mag
    # Sign-magnitude encodings have monotonic positive magnitudes. Both zeros
    # have a single comparison key; negatives reverse the magnitude ordering.
    key = lambda x: -(x&mag) if x&sign else x&mag
    x,y = key(a),key(b)
    match = {"fcmeq":x==y,"fcmge":x>=y,"fcmgt":x>y,"fcmle":x<=y,"fcmlt":x<y,"facge":x>=y,"facgt":x>y}[op]
    return (1<<bits)-1 if match else 0,flags

def edges(bits):
    fraction,bias = {16:(10,15),32:(23,127),64:(52,1023)}[bits]
    sign,inf,quiet,one = 1<<(bits-1),(2*bias+1)<<fraction,1<<(fraction-1),bias<<fraction
    return (0,sign,one,sign|one,one-1,one+1,1,sign|1,(1<<fraction)-1,sign|((1<<fraction)-1),1<<fraction,sign|(1<<fraction),inf,sign|inf,inf|quiet|3,sign|inf|quiet|5,inf|1,sign|inf|3)

def probe():
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
    import unicorn.arm64_const as r
    count = 0
    for bits,q,scalar in SHAPES:
        for zero in (0,1):
            for operation,op in enumerate(ZERO_OPS if zero else REG_OPS):
                for fpcr in (0,1<<19,1<<24,1<<25):
                    for a,b in ((edges(bits)[14],0),(edges(bits)[16],0),(1,0),(edges(bits)[3],edges(bits)[2])):
                        cpu = Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
                        cpu.mem_map(0x1000,4096);cpu.mem_write(0x1000,struct.pack("<I",encode(bits,q,scalar,zero,operation)))
                        cpu.reg_write(r.UC_ARM64_REG_Q1,a);cpu.reg_write(r.UC_ARM64_REG_Q2,b)
                        cpu.reg_write(r.UC_ARM64_REG_FPCR,fpcr);cpu.reg_write(r.UC_ARM64_REG_FPSR,0x08000002)
                        cpu.emu_start(0x1000,0x1004,count=1)
                        value,flags = reference(a,0 if zero else b,bits,op,fpcr)
                        # Remaining zero lanes compare independently.
                        active = 1 if scalar else (128 if q else 64)//bits
                        for lane in range(1,active):
                            extra,exc = reference(0,0,bits,op,fpcr);value |= extra<<(lane*bits);flags |= exc
                        got = (cpu.reg_read(r.UC_ARM64_REG_Q0),cpu.reg_read(r.UC_ARM64_REG_FPSR))
                        if got!=(value,0x08000002|flags):
                            raise ValueError((bits,q,scalar,zero,op,hex(fpcr),hex(a),hex(b),got,(value,0x08000002|flags)))
                        count += 1
    print(f"PASS {count} tiny MAX compare shape/NaN/FZ/FZ16/DN probes")

def corpus(case_type,family=None):
    rng,cases,physical = random.Random(0xC04A264),[],(0,31,30,3)
    for bits,q,scalar in SHAPES:
        values = edges(bits)
        for zero in (0,1):
            for operation,op in enumerate(ZERO_OPS if zero else REG_OPS):
                if family is not None and op!=family: continue
                for alias,(rd,rn,rm) in enumerate(ALIASES):
                    for control,fpcr in enumerate(CONTROLS):
                        for profile in range(20):
                            vectors = [rng.getrandbits(128) for _ in range(4)]
                            lhs = [values[(profile+lane)%len(values)] for lane in range(128//bits)]
                            rhs = [values[(profile*3+lane+alias)%len(values)] for lane in range(128//bits)]
                            if profile==18:
                                # Inactive sNaN/subnormal must never alter FPSR.
                                active = 1 if scalar else (128 if q else 64)//bits
                                lhs = [values[2] if lane<active else values[16] for lane in range(128//bits)]
                                rhs = [values[2] if lane<active else 1 for lane in range(128//bits)]
                            if profile==19:
                                lhs = [rng.getrandbits(bits) for _ in lhs];rhs = [rng.getrandbits(bits) for _ in rhs]
                            vectors[rn] = sum(x<<(lane*bits) for lane,x in enumerate(lhs))
                            if rn!=rm and not zero: vectors[rm] = sum(x<<(lane*bits) for lane,x in enumerate(rhs))
                            seeds = {1+slot*2+part:(v>>(part*64))&MASK64 for slot,v in enumerate(vectors) for part in range(2)}
                            seeds.update({27:0x08000002,28:fpcr})
                            initialize = tuple(word for slot,reg in enumerate(physical) for word in (0x9E670000|((slot*2+1)<<5)|reg,0x9EAF0000|((slot*2+2)<<5)|reg))
                            word = encode(bits,q,scalar,zero,operation,physical[rd],physical[rn],physical[rm])
                            capture = (0xD53B443B,0xD53B441C)+tuple(0x3D8003A0|(slot<<10)|reg for slot,reg in enumerate(physical))
                            name = f"fpcompare_{op}_e{bits}_q{q}_s{scalar}_z{zero}_a{alias}_c{control}_p{profile}"
                            cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,name,seeds,0xB0000000,"fpcompare",bytes(range(64))))
    count = 64000 if family is None else 6400*sum(op==family for op in REG_OPS+ZERO_OPS)
    if len(cases)!=count or len({case.name for case in cases})!=len(cases): raise ValueError("Compare coverage changed")
    return cases

def expected(case):
    import arm64_differential as d
    _,op,width,qtext,stext,ztext,atext,_,_ = case.name.split("_")
    bits,q,scalar,zero = int(width[1:]),int(qtext[1:]),int(stext[1:]),int(ztext[1:])
    rd,rn,rm = ALIASES[int(atext[1:])]
    vectors = [case.seeds[n]|(case.seeds[n+1]<<64) for n in (1,3,5,7)]
    a,b,result,flags = vectors[rn],vectors[rm],0,case.seeds[27]
    for lane in range(1 if scalar else (128 if q else 64)//bits):
        value,exc = reference((a>>(lane*bits))&((1<<bits)-1),0 if zero else (b>>(lane*bits))&((1<<bits)-1),bits,op,case.seeds[28])
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
            raise ValueError(f"{case.name}: independent SIMD compare mask/FPSR/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} SIMD compare/quiet-signaling/NaN/FZ/FZ16/IDC/alias/full ELF/Capstone/Unicorn cases")
    return 0


if __name__=="__main__":
    parser = argparse.ArgumentParser();parser.add_argument("--probe",action="store_true");parser.add_argument("--verify",action="store_true");parser.add_argument("--family",choices=tuple(set(REG_OPS+ZERO_OPS)))
    args = parser.parse_args()
    if args.probe: probe()
    elif args.verify: verify(args.family)
    else: parser.error("choose --probe or --verify")
