"""S/D vector FSQRT/FRINT integer-root and rounding oracle, full ELF snapshots."""

import argparse
import math
import random
import struct

OPS = ("fsqrt","frintn","frintm","frintp","frintz","frinta","frintx","frinti")
BASES = (0x2EA1F800,0x0E218800,0x0E219800,0x0EA18800,0x0EA19800,0x2E218800,0x2E219800,0x2EA19800)
ALIASES = ((0,1),(1,1),(2,1))
CONTROLS = (0,1 << 22,2 << 22,3 << 22,1 << 24,1 << 25,(1 << 19) | (1 << 25),1 << 19)
MASK64 = (1 << 64)-1


def reference(value,bits,op,fpcr):
    fraction_bits,bias = (23,127) if bits==32 else (52,1023)
    sign,maximum_exponent = value & (1 << (bits-1)),2*bias+1
    exponent,fraction = (value >> fraction_bits)&maximum_exponent,value & ((1 << fraction_bits)-1)
    infinity,quiet,scale,flags = maximum_exponent << fraction_bits,1 << (fraction_bits-1),fraction_bits+bias-1,0
    if exponent==maximum_exponent and fraction:
        return (infinity | quiet) if fpcr & (1 << 25) else value | quiet,0 if fraction & quiet else 1
    if not exponent and fraction and fpcr & (1 << 24):
        value,fraction,flags = sign,0,128
    if not value & ~(1 << (bits-1)):
        return value,flags
    if exponent==maximum_exponent:
        return (infinity | quiet,flags | 1) if sign and op=="fsqrt" else (value,flags)
    magnitude = ((fraction | (1 << fraction_bits)) << (exponent-1)) if exponent else fraction
    mode = (fpcr >> 22)&3
    if op=="fsqrt":
        if sign:
            return infinity | quiet,flags | 1
        target = magnitude << scale
        root = math.isqrt(target)
        shift = max(0,root.bit_length()-1-fraction_bits)
        rounded = root >> shift
        exact = (rounded << shift)**2==target
        increment = mode==1 and not exact
        if mode==0:
            midpoint = ((2*rounded+1) << shift)**2
            increment = 4*target>midpoint or (4*target==midpoint and bool(rounded&1))
        rounded += increment
        if rounded>=1 << (fraction_bits+1):
            rounded >>= 1
            shift += 1
        return ((shift+1) << fraction_bits) | (rounded & ((1 << fraction_bits)-1)),flags | (0 if exact else 16)
    mode = {"frintn":0,"frintp":1,"frintm":2,"frintz":3,"frinta":4}.get(op,mode)
    rounded,remainder = divmod(magnitude,1 << scale)
    if remainder:
        increment = (mode==1 and not sign) or (mode==2 and bool(sign))
        if mode in (0,4):
            increment = remainder>1 << (scale-1) or (remainder==1 << (scale-1) and (mode==4 or bool(rounded&1)))
        rounded += increment
    if remainder and op=="frintx":
        flags |= 16
    if not rounded:
        return sign,flags
    top = rounded.bit_length()-1
    significand = (rounded << fraction_bits) >> top
    return sign | ((top+bias) << fraction_bits) | (significand & ((1 << fraction_bits)-1)),flags


def corpus(case_type):
    rng,cases,physical = random.Random(0xF032640026),[],(0,31,30,3)
    for bits,q in ((32,0),(32,1),(64,1)):
        fraction,bias = (23,127) if bits==32 else (52,1023)
        sign,infinity,quiet = 1 << (bits-1),(2*bias+1) << fraction,1 << (fraction-1)
        one,two,half,minimum = bias << fraction,(bias+1) << fraction,(bias-1) << fraction,1 << fraction
        profiles = (
            (0,sign,one,two),
            (half,sign | half,one | quiet,sign | (one | quiet)),
            (half-1,half+1,sign | (half-1),sign | (half+1)),
            (1,sign | 1,minimum-1,sign | (minimum-1)),
            (minimum,sign | minimum,minimum+1,sign | (minimum+1)),
            (infinity | quiet | 3,sign | infinity | quiet | 5,infinity | 1,sign | infinity | 3),
            (infinity,sign | infinity,infinity-1,sign | (infinity-1)),
            ((bias+fraction-1) << fraction,((bias+fraction-1) << fraction)+1,
             (bias+fraction) << fraction,((bias+fraction) << fraction)+1),
            (one-1,one+1,(bias+2) << fraction,((bias+2) << fraction)+1),
            (one,two,((bias+3) << fraction) | (1 << (fraction-3)),((bias+4) << fraction)),
            (one,one,infinity | 1,1),
        )
        for operation,op in enumerate(OPS):
            for alias,(rd,rn) in enumerate(ALIASES):
                for control,fpcr in enumerate(CONTROLS):
                    for profile in range(12):
                        vectors = [rng.getrandbits(128) for _ in range(4)]
                        values = [profiles[profile][(lane+alias)%4] if profile<11 else rng.getrandbits(bits) for lane in range(128//bits)]
                        if profile==10 and not q:
                            values = [one if lane<64//bits else infinity | 1 for lane in range(128//bits)]
                        vectors[rn] = sum(value << (lane*bits) for lane,value in enumerate(values))
                        seeds = {1+slot*2+part:(vector >> (part*64)) & MASK64
                                 for slot,vector in enumerate(vectors) for part in range(2)}
                        seeds.update({27:0x08000002,28:fpcr})
                        initialize = tuple(word for slot,reg in enumerate(physical) for word in
                                           (0x9E670000 | ((slot*2+1) << 5) | reg,0x9EAF0000 | ((slot*2+2) << 5) | reg))
                        word = BASES[operation] | (q << 30) | ((bits==64) << 22) | (physical[rn] << 5) | physical[rd]
                        capture = (0xD53B443B,0xD53B441C) + tuple(0x3D8003A0 | (slot << 10) | reg for slot,reg in enumerate(physical))
                        name = f"vecunary_{op}_e{bits}_q{q}_a{alias}_c{control}_p{profile}"
                        cases.append(case_type(name,(0xD51B443B,0xD51B441C)+initialize+(word,)+capture,
                                               name,seeds,0xB0000000,"vecunary",bytes(range(64))))
    if len(cases)!=6912 or len({case.name for case in cases})!=len(cases):
        raise ValueError(f"Vector S/D unary coverage changed: {len(cases)}")
    return cases


def expected(case):
    import arm64_differential as differential
    _,op,width,q_text,alias_text,_,_ = case.name.split("_")
    bits,q = int(width[1:]),int(q_text[1:])
    rd,rn = ALIASES[int(alias_text[1:])]
    vectors = [case.seeds[n] | (case.seeds[n+1] << 64) for n in (1,3,5,7)]
    source,result,fpsr = vectors[rn],0,case.seeds[27]
    for lane in range((128 if q else 64)//bits):
        value,flags = reference((source >> (lane*bits)) & ((1 << bits)-1),bits,op,case.seeds[28])
        result |= value << (lane*bits);fpsr |= flags
    vectors[rd] = result
    registers = {f"x{n}":case.seeds.get(n,0x100+n) for n in range(29)}
    registers.update({"x27":fpsr,"x28":case.seeds[28],"x29":differential.MEMORY_BASE,"x30":differential.DATA_BASE})
    return {"memory":b"".join(vector.to_bytes(16,"little") for vector in vectors),
            "registers":registers,"nzcv":case.nzcv,"fpsr":fpsr,"fpcr":case.seeds[28]}


def verify():
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case)
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
            raise ValueError(f"{case.name}: independent S/D integer root and rounding/64-byte fixture differs from Unicorn")
        for register, value in wanted["registers"].items():
            if int(state["registers"][register], 16) != value:
                raise ValueError(f"{case.name}: {register} differs from independent reference")
    print(f"PASS {len(cases)} vector S/D unary/integer root/FRINT/NaN/FZ/IDC/full ELF/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
