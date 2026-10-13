#!/usr/bin/env python3
"""Independent A64 CFG ELF/math/Unicorn/SwiftVM full-snapshot regression.

Guest function statistics prove whole-function integration, not shared-frame
Phi lowering: current x64 function compilation does not run uniform SSA.
"""
from __future__ import annotations
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import struct
import subprocess
import sys
import time
import arm64_differential as d

MODES = {f"base{base}_uniform{uniform}": {
    "SVM_FUNC_BASE": str(base), "SVM_UNIFORM_ELIM": str(uniform),
    "SVM_FUNC_STATS": "1", "SVM_ENABLE_JIT": "1"}
    for base in (0, 1) for uniform in (0, 1)}

def modes_for(matrix):
    if matrix == "cfg4":
        # Pin the legacy matrix to cache OFF, independent of ambient settings.
        return {name: {**env, "SVM_X64_SCALAR_CACHE": "0"} for name, env in MODES.items()}
    if matrix == "cache8":
        return {f"{name}_cache{cache}": {**env, "SVM_X64_SCALAR_CACHE": str(cache)}
                for name, env in MODES.items() for cache in (0, 1)}
    raise ValueError("unknown matrix: " + matrix)
STATS = re.compile(rb"\[func-stats\] isa=aarch64 attempted=(\d+) compiled=(\d+) fallback_exception=(\d+) fallback_block_cap=(\d+) compiled_blocks=(\d+) avg_blocks=([\d.]+)")
STAT_KEYS = ("attempted", "compiled", "fallback_exception", "fallback_block_cap", "compiled_blocks")
EMISSION = re.compile(rb"\[x64-emission\] pc=(0x[0-9a-fA-F]+) scalar_cache_reads=(\d+) scalar_cache_writes=(\d+) code_bytes=(\d+) frame_bytes=(\d+)(?: coalesced_binary=(\d+))?")
EMISSION_KEYS = ("scalar_cache_reads", "scalar_cache_writes", "code_bytes", "frame_bytes", "coalesced_binary")
FLAGS_LIVENESS = re.compile(rb"\[x64-flags-liveness\] pc=(0x[0-9a-fA-F]+) full=(\d+) narrowed=(\d+) barriers=(\d+)")
FLAGS_LIVENESS_KEYS = ("full", "narrowed", "barriers")
MASK = d.MASK64

def utc(): return datetime.now(timezone.utc).isoformat()
def sha(raw): return hashlib.sha256(raw).hexdigest()
def file_sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for data in iter(lambda: stream.read(1048576), b""): h.update(data)
    return h.hexdigest()
def checkpoint(path, data):
    temporary = path.with_suffix(".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(data, stream, indent=2); stream.write("\n")
        stream.flush(); os.fsync(stream.fileno())
    os.replace(temporary, path)
def sources(): return {str(p): file_sha(p) for p in (Path(__file__).resolve(), Path(d.__file__).resolve())}

class Program:
    def __init__(self): self.words, self.labels, self.fixups = [], {}, []
    def emit(self, *words): self.words.extend(words)
    def label(self, name):
        if name in self.labels: raise ValueError("duplicate label")
        self.labels[name] = len(self.words)
    def branch(self, base, label, bits=19, shift=5):
        self.fixups.append((len(self.words), base, label, bits, shift)); self.words.append(0)
    def finish(self):
        for at, base, label, bits, shift in self.fixups:
            delta = self.labels[label] - at
            if not -(1 << (bits - 1)) <= delta < (1 << (bits - 1)): raise ValueError("branch range")
            self.words[at] = base | ((delta & ((1 << bits) - 1)) << shift)
        return tuple(self.words)

def arithmetic(a, b, width, subtract=False):
    mask, sign = (1 << width) - 1, 1 << (width - 1)
    a &= mask; b &= mask
    result = (a - b if subtract else a + b) & mask
    carry = a >= b if subtract else a + b > mask
    overflow = bool(((a ^ b) if subtract else ~(a ^ b)) & (a ^ result) & sign)
    return result, (int(bool(result & sign)) << 31) | (int(result == 0) << 30) | (int(carry) << 29) | (int(overflow) << 28)

def condition(code, flags):
    n, z, c, v = (bool(flags & (1 << bit)) for bit in (31, 30, 29, 28))
    tests = (z, c, n, v, c and not z, n == v, not z and n == v, True)
    return tests[code // 2] if code % 2 == 0 or code >= 14 else not tests[code // 2]

def initial(case):
    regs = [case.seeds.get(i, 0x100 + i) for i in range(29)] + [d.MEMORY_BASE, d.DATA_BASE]
    return regs, bytearray(case.memory), case.nzcv

def corpus():
    result = []
    values = (0, 1, MASK, 1 << 63, (1 << 63) - 1, 0x100000000, 0xffffffff, 0x123456789abcdef0)
    pairs = ((0, 0), (0, 1), (MASK, 1), (1 << 63, MASK), ((1 << 63)-1, 1),
             (0x80000000, 1), (0xffffffff, 1), (0x123456789abcdef0, 0xfedcba9876543210))
    fixture = bytes((0x80 + 17 * i) & 255 for i in range(64))
    def add(family, width, params, seeds, flags, body, math_fn):
        # Explicit architectural FP controls, captured as GPRs in the existing protocol.
        seeds = dict(seeds, **{})
        seeds.update({25: 0, 26: 0})
        instructions = (0xD51B4439, 0xD51B441A) + body + (0xD53B443B, 0xD53B441C)
        case = d.Case(f"cfg_{family}_w{width}_{len(result):04d}", instructions,
                      json.dumps(params, sort_keys=True), seeds, flags, "cfg_" + family, fixture)
        regs, memory, nzcv = initial(case)
        regs, memory, nzcv = math_fn(regs, memory, nzcv)
        regs[27] = regs[28] = 0
        raw = d.MAGIC + struct.pack("<Q31QQ", d.case_id(case), *regs, nzcv) + bytes(memory)
        d.decode_snapshot(raw, case)
        result.append((case, raw, {"family": family, "width": width, **params}))
    for width in (32, 64):
        sf = (width == 64) << 31; mask = (1 << width) - 1
        for invert in (False, True):
            for value in values:
                for flags in (0, 0x30000000, 0x60000000, 0xf0000000):
                    p = Program(); p.branch(sf | 0x34000000 | (int(invert) << 24), "true")
                    p.emit(d.rr(sf | 0x4b000000, 2, 0, 1)); p.branch(0x14000000, "join", 26, 0)
                    p.label("true"); p.emit(d.rr(sf | 0x0b000000, 2, 0, 1))
                    p.label("join"); p.emit(d.rr(0xca000000, 7, 2, 1), 0xf90003a2, 0xf90007a7)
                    a, b = value, values[(values.index(value) + 3) % len(values)]
                    def math_fn(regs, memory, nzcv, a=a, b=b, mask=mask, invert=invert):
                        taken = bool(a & mask) if invert else not bool(a & mask)
                        regs[2] = (a + b if taken else a - b) & mask
                        regs[7] = regs[2] ^ b
                        memory[:16] = struct.pack("<QQ", regs[2], regs[7]); return regs, memory, nzcv
                    add("cbnz" if invert else "cbz", width, {"a": a, "b": b}, {0:a, 1:b}, flags, p.finish(), math_fn)
        for cond in range(14):
            for a, b in pairs:
                for flags in (0, 0xf0000000):
                    p = Program(); p.emit(d.rr(sf | 0x6b000000, 31, 0, 1)); p.branch(0x54000000 | cond, "true")
                    p.emit(d.rr(sf | 0x6b000000, 2, 0, 1)); p.branch(0x14000000, "join", 26, 0)
                    p.label("true"); p.emit(d.rr(sf | 0x2b000000, 2, 0, 1))
                    p.label("join"); p.emit(d.rr(0x9a000000, 7, 2, 1), 0xf90013a7)
                    def math_fn(regs, memory, nzcv, a=a, b=b, width=width, cond=cond):
                        _, compare = arithmetic(a, b, width, True)
                        taken = condition(cond, compare)
                        regs[2], nzcv = arithmetic(a, b, width, not taken)
                        regs[7] = (regs[2] + b + bool(nzcv & 0x20000000)) & MASK
                        memory[32:40] = struct.pack("<Q", regs[7]); return regs, memory, nzcv
                    add("flags", width, {"condition":cond, "a":a, "b":b}, {0:a, 1:b}, flags, p.finish(), math_fn)
        for count in (0, 1, 2, 3, 7, 13, 31, 63, 127):
            for start in values[:6]:
                for flags in (0x30000000, 0xf0000000):
                    step = values[(values.index(start) + 1) % len(values)]
                    p = Program(); p.branch(sf | 0x34000000, "exit")
                    p.label("loop"); p.emit(d.rr(sf | 0x0b000000, 2, 2, 1), sf | 0x71000400)
                    p.branch(0x54000001, "loop"); p.label("exit")
                    p.emit(d.rr(0xca000000, 7, 2, 1), 0xf90003a2, 0xf90007a7)
                    def math_fn(regs, memory, nzcv, count=count, start=start, step=step, mask=mask):
                        if count: regs[0], regs[2], nzcv = 0, (start + count * step) & mask, 0x60000000
                        regs[7] = regs[2] ^ step; memory[:16] = struct.pack("<QQ", regs[2], regs[7]); return regs, memory, nzcv
                    add("loop", width, {"count":count, "start":start, "step":step}, {0:count, 1:step, 2:start}, flags, p.finish(), math_fn)
    for invert in (False, True):
        for index, value in enumerate(values):
            for flags in (0, 0x30000000, 0x60000000, 0xf0000000):
                lanes = [values[(index + j) % len(values)] for j in range(4)]
                p = Program(); p.emit(0x9e670060, 0x9eaf0080, 0x9e6700a1, 0x9eaf00c1)
                p.branch(0xb4000000 | (int(invert) << 24), "true")
                p.emit(0x4ea11c22); p.branch(0x14000000, "join", 26, 0)
                p.label("true"); p.emit(0x4ea01c02)
                p.label("join"); p.emit(0x4ee18443, 0x3d8003a2, 0x3d8007a3)
                def math_fn(regs, memory, nzcv, lanes=lanes, value=value, invert=invert):
                    chosen = lanes[:2] if (bool(value) if invert else not bool(value)) else lanes[2:]
                    summed = [(chosen[i] + lanes[i+2]) & MASK for i in range(2)]
                    memory[:32] = struct.pack("<4Q", *chosen, *summed); return regs, memory, nzcv
                add("vector", 128, {"invert":invert, "selector":value, "lanes":lanes},
                    {0:value, **{3+j:lanes[j] for j in range(4)}}, flags, p.finish(), math_fn)
    if len(result) != 856: raise ValueError(f"scope changed: {len(result)}")
    return result

def execute_elf(case, elf):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    import unicorn.arm64_const as r
    code = struct.unpack_from("<IIQQQQQQ", elf, 64)
    data = struct.unpack_from("<IIQQQQQQ", elf, 120)
    if len(list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(elf[4096:code[5]], d.CODE_BASE+4096))) * 4 != code[5]-4096:
        raise ValueError("Capstone rejected ELF instructions")
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM); cpu.ctl_set_cpu_model(r.UC_CPU_ARM64_MAX)
    cpu.mem_map(d.CODE_BASE, 65536); cpu.mem_map(d.DATA_BASE, 65536)
    cpu.mem_write(d.CODE_BASE, elf[:code[5]]); cpu.mem_write(d.DATA_BASE, elf[data[2]:data[2]+data[5]])
    snapshots, exits = [], []
    def interrupt(emulator, number, unused):
        syscall = emulator.reg_read(r.UC_ARM64_REG_X8)
        if syscall == 64:
            if emulator.reg_read(r.UC_ARM64_REG_X0) != 1 or emulator.reg_read(r.UC_ARM64_REG_X2) != d.SNAPSHOT_SIZE: raise ValueError("invalid snapshot write")
            snapshots.append(bytes(emulator.mem_read(emulator.reg_read(r.UC_ARM64_REG_X1), d.SNAPSHOT_SIZE)))
            emulator.reg_write(r.UC_ARM64_REG_X0, d.SNAPSHOT_SIZE)
        elif syscall == 93:
            if emulator.reg_read(r.UC_ARM64_REG_X0): raise ValueError("harness error exit")
            exits.append(True); emulator.emu_stop()
        else: raise ValueError(f"unexpected syscall {syscall}/{number}")
    cpu.hook_add(UC_HOOK_INTR, interrupt); cpu.emu_start(d.CODE_BASE+4096, 0, count=10000)
    if len(snapshots) != 1 or len(exits) != 1: raise ValueError("snapshot/exit count")
    d.decode_snapshot(snapshots[0], case); return snapshots[0]


def cache_stress_corpus():
    """Eight live scalar producers across memory/FP returning helpers.

    This covers guest helper returns, not arbitrary user callback mutation,
    reentry, signals, or fault recovery. Those require independent C++ tests.
    """
    result = []
    values = (0, 1, MASK, 1 << 63, (1 << 63)-1, 0x100000000,
              0xffffffff, 0x123456789abcdef0)
    fixture = bytes((0x80 + 17*i) & 255 for i in range(64))
    for width in (32, 64):
        sf = (width == 64) << 31
        for profile in range(8):
            for flags in (0, 0x30000000, 0x60000000, 0xf0000000):
                for fp_helper in (False, True):
                    seeds = {i: values[(profile+i) % 8] for i in range(8)}
                    seeds.update({20: 0x3ff0000000000000, 21: 0x4000000000000000,
                                  25: 0, 26: 0})
                    p = Program()
                    p.emit(0xD51B4439, 0xD51B441A)
                    for j in range(8): p.emit(d.rr(sf | 0x0b000000, 8+j, j, 1))
                    p.emit(d.rr(sf | 0x2b000000, 31, 0, 1), 0xf94003b0)
                    if fp_helper:
                        p.emit(0x9e670000 | (20 << 5), 0x9e670000 | (21 << 5) | 1,
                               0x1e602800 | (1 << 16) | 2,
                               0x9e660000 | (2 << 5) | 22)
                    p.emit(d.rr(sf | 0x2a000000, 17, 31, 16))
                    for j in range(8): p.emit(d.rr(sf | 0x3a000000, 17, 17, 8+j))
                    p.emit(0xf90007b1)
                    p.branch(0xb4000011, "zero")
                    p.emit(d.rr(0xca000000, 18, 17, 0)); p.branch(0x14000000, "join", 26, 0)
                    p.label("zero"); p.emit(d.rr(0x8b000000, 18, 17, 0))
                    p.label("join"); p.emit(0xf9000bb2, 0xD53B443B, 0xD53B441C)
                    case = d.Case(f"cfgcache_pressure_w{width}_{len(result):04d}", p.finish(),
                                  json.dumps({"profile":profile,"fp_helper":fp_helper}), seeds,
                                  flags, "cfgcache_pressure", fixture)
                    regs, memory, nzcv = initial(case)
                    mask = (1 << width)-1
                    for j in range(8): regs[8+j] = (regs[j]+regs[1]) & mask
                    _, nzcv = arithmetic(regs[0], regs[1], width)
                    regs[16] = int.from_bytes(memory[:8], "little")
                    if fp_helper: regs[22] = 0x4008000000000000  # exact 1.0 + 2.0
                    regs[17] = regs[16] & mask
                    for j in range(8):
                        a, b, carry = regs[17], regs[8+j], bool(nzcv & 0x20000000)
                        total = a+b+carry; value = total & mask
                        signed = lambda x: x-(1 << width) if x & (1 << (width-1)) else x
                        full_signed = signed(a)+signed(b)+carry
                        overflow = not -(1 << (width-1)) <= full_signed < (1 << (width-1))
                        nzcv = (int(bool(value & (1 << (width-1)))) << 31) | (int(value == 0) << 30) | (int(total > mask) << 29) | (int(overflow) << 28)
                        regs[17] = value
                    regs[18] = (regs[17]+regs[0]) & MASK if regs[17] == 0 else regs[17]^regs[0]
                    memory[8:24] = struct.pack("<QQ", regs[17], regs[18])
                    regs[27] = regs[28] = 0
                    raw = d.MAGIC+struct.pack("<Q31QQ", d.case_id(case), *regs, nzcv)+bytes(memory)
                    d.decode_snapshot(raw,case)
                    result.append((case,raw,{"family":"cache_pressure","width":width,"profile":profile,"fp_helper":fp_helper,"live_scalar_producers":8}))
    if len(result) != 128: raise ValueError("cache pressure scope changed")
    straight = []
    for case, raw, parameters in result:
        # Both zero/nonzero diamond paths have the same XOR result when x17=0.
        # Removing that diamond creates a guest block ending in harness SVC,
        # retaining eight values across helpers without FunctionPlan branches.
        instructions = case.instructions[:-7] + (
            d.rr(0xca000000, 18, 17, 0), 0xf9000bb2, 0xD53B443B, 0xD53B441C)
        single = d.Case(case.name.replace("pressure", "straight"), instructions,
                        case.assembly, case.seeds, case.nzcv, "cfgcache_straight", case.memory)
        single_raw = raw[:8] + struct.pack("<Q", d.case_id(single)) + raw[16:]
        straight.append((single, single_raw, {**parameters, "family":"cache_straight",
                                              "requires_multiblock":False}))
    result += straight
    if len(result) != 256: raise ValueError("cache straight/diamond scope changed")
    return result

def compare(expected, actual, case):
    parsed = d.decode_snapshot(actual, case)
    errors = d.differences(d.decode_snapshot(expected, case), parsed)
    if expected != actual: errors.append("complete raw snapshot differs")
    return errors

def run(args, cases):
    import capstone, unicorn
    modes = modes_for(args.matrix)
    modes = {name: {**env, "SVM_FLAG_FULL_ELIM": args.flags_liveness} for name, env in modes.items()}
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    fs = subprocess.check_output(["stat", "-f", "-c", "%T", str(output)], text=True).strip()
    if fs not in ("ext2/ext3", "ext4"): raise ValueError("evidence directory must be ext4")
    runner = args.runner.resolve(strict=True); rs = runner.stat()
    if not stat.S_ISREG(rs.st_mode) or rs.st_mode & 0o222 or not os.access(runner, os.X_OK): raise ValueError("runner must be read-only executable")
    fd = os.open(runner, os.O_RDONLY); pinned = f"/proc/self/fd/{fd}"
    if file_sha(pinned) != args.runner_sha256: raise ValueError("runner SHA256 mismatch")
    startup = sources()
    report = {"version":1, "scope":"a64_cfg_856_"+args.matrix+("_cache_stress" if args.cache_stress else ""), "planned_cases":len(cases), "planned_processes":len(cases)*len(modes),
        "attempted":0, "math_checked":0, "unicorn_executed":0, "executed":0, "passed_cases":0, "failed_cases":0,
        "complete":False, "success":False, "started_utc":utc(), "filesystem":fs, "modes":modes,
        "source_sha256_startup":startup, "runner":{"path":str(runner), "sha256_startup":args.runner_sha256,
        "device":rs.st_dev, "inode":rs.st_ino, "size":rs.st_size, "mode":oct(stat.S_IMODE(rs.st_mode))},
        "oracle":{"unicorn":unicorn.__version__, "capstone":capstone.__version__, "cpu_model":"UC_CPU_ARM64_MAX"},
        "process_evidence":{"pid":os.getpid(), "parent_pid":os.getppid(), "argv":sys.argv, "platform":platform.platform(), "python":sys.version},
        "evidence_boundary":"Function statistics require successful multiblock HIR integration. Guest decoding retains uniform loads/stores; this does not claim shared-frame Phi lowering. Hand-built HIR tests cover that path."}
    report_path=output/"report.json"; checkpoint(report_path, report)
    (output/"elves").mkdir(); (output/"failures").mkdir()
    streams = {name:(output/(name+".bin")).open("wb") for name in ("math", "unicorn", *modes, "stderr")}
    journal=(output/"cases.jsonl").open("w", encoding="utf-8")
    aggregates={mode:Counter() for mode in modes}; process_ids=set()
    emission_totals={mode:Counter() for mode in modes}
    straight_emission_totals={mode:Counter() for mode in modes}
    flags_totals={mode:Counter(dict.fromkeys(FLAGS_LIVENESS_KEYS, 0)) for mode in modes}
    straight_flags_totals={mode:Counter(dict.fromkeys(FLAGS_LIVENESS_KEYS, 0)) for mode in modes}
    try:
        for index, (case, math_raw, parameters) in enumerate(cases):
            report["attempted"] += 1
            elf=d.make_elf(case); elf_path=output/"elves"/(case.name+".elf"); elf_path.write_bytes(elf)
            row={"index":index, "name":case.name, "parameters":parameters, "elf_path":str(elf_path.relative_to(output)), "elf_sha256":sha(elf), "elf_size":len(elf), "started_utc":utc(), "errors":[], "modes":{}}
            def append(name, raw):
                stream=streams[name]; entry={"offset":stream.tell(), "size":len(raw), "sha256":sha(raw)}
                stream.write(raw); return entry
            row["math"]=append("math", math_raw); report["math_checked"]+=1
            try:
                uc_raw=execute_elf(case, elf); row["unicorn"]=append("unicorn", uc_raw); report["unicorn_executed"]+=1
                row["errors"] += compare(math_raw, uc_raw, case)
                if row["errors"]: raise ValueError("Unicorn/math disagreement")
                for mode, overrides in modes.items():
                    env={k:v for k,v in os.environ.items() if not k.startswith(("SVM_", "SWIFT_"))}; env.update(overrides)
                    command=[str(runner), *args.runner_arg, str(elf_path)]
                    process=subprocess.Popen(command, executable=pinned, pass_fds=(fd,), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    report["executed"]+=1; process_ids.add(process.pid)
                    entry={"pid":process.pid, "command":command, "environment":overrides, "started_utc":utc(), "device":rs.st_dev, "inode":rs.st_ino}
                    try:
                        entry["proc_exe"]=os.readlink(f"/proc/{process.pid}/exe")
                        entry["proc_cmdline"]=Path(f"/proc/{process.pid}/cmdline").read_bytes().decode("utf-8", "replace").split("\0")
                    except (FileNotFoundError, ProcessLookupError): entry["proc_inspection"]="exited before inspection"
                    try: stdout, stderr=process.communicate(timeout=args.timeout)
                    except subprocess.TimeoutExpired:
                        process.kill(); stdout, stderr=process.communicate(); row["errors"].append(mode+": timeout")
                    entry.update({"returncode":process.returncode, "completed_utc":utc(), "raw":append(mode, stdout), "stderr":append("stderr", stderr)})
                    parsed=list(STATS.finditer(stderr))
                    if len(parsed)!=1: row["errors"].append(mode+": missing/duplicate function statistics")
                    else:
                        stats={key:int(value) for key,value in zip(STAT_KEYS, parsed[0].groups()[:5])}; entry["function_stats"]=stats
                        aggregates[mode].update(stats)
                        if overrides["SVM_FUNC_BASE"]=="1":
                            min_blocks = stats["compiled"] + int(parameters.get("requires_multiblock", True))
                            if not stats["compiled"] or stats["compiled_blocks"] < min_blocks or stats["fallback_exception"] or stats["fallback_block_cap"]:
                                row["errors"].append(mode+": whole-function integration not proven/fallback")
                        elif stats["attempted"] or stats["compiled"]: row["errors"].append(mode+": block-only mode unexpectedly compiled function")
                    emissions = [{"pc":match.group(1).decode(), **{
                        key:int(value or 0) for key,value in zip(EMISSION_KEYS,match.groups()[1:])}}
                        for match in EMISSION.finditer(stderr)]
                    entry["x64_emissions"] = emissions
                    flag_sites = [{"pc": match.group(1).decode(), **dict(zip(FLAGS_LIVENESS_KEYS, map(int, match.groups()[1:])))}
                                  for match in FLAGS_LIVENESS.finditer(stderr)]
                    entry["flags_liveness_sites"] = flag_sites
                    flag_counts = Counter(dict.fromkeys(FLAGS_LIVENESS_KEYS, 0))
                    for site in flag_sites:
                        if site["narrowed"] > site["full"]:
                            row["errors"].append(mode+": invalid flags-liveness counts")
                        flag_counts.update({key: site[key] for key in FLAGS_LIVENESS_KEYS})
                    entry["flags_liveness_totals"] = dict(flag_counts)
                    if args.flags_liveness == "0" and flag_sites:
                        row["errors"].append(mode+": disabled flags-liveness pass ran")
                    if args.flags_liveness == "1" and not flag_sites:
                        row["errors"].append(mode+": flags-liveness production integration not observed")
                    flags_totals[mode].update(flag_counts)
                    if parameters["family"] == "cache_straight":
                        straight_flags_totals[mode].update(flag_counts)
                    counts = Counter({key:0 for key in EMISSION_KEYS})
                    for emitted in emissions:
                        counts.update({key:emitted[key] for key in EMISSION_KEYS})
                    entry["x64_emission_totals"] = dict(counts)
                    entry["emission_evidence_boundary"] = "static emitted sites after successful Translate; not dynamic hits or proof of publication"
                    emission_totals[mode].update(counts)
                    if parameters["family"] == "cache_straight":
                        straight_emission_totals[mode].update(counts)
                    if args.matrix == "cache8":
                        if not emissions: row["errors"].append(mode+": missing x64 emission statistics")
                        if overrides["SVM_X64_SCALAR_CACHE"] == "0" and (counts["scalar_cache_reads"] or counts["scalar_cache_writes"] or counts["coalesced_binary"]):
                            row["errors"].append(mode+": cache OFF emitted cache read/write sites")
                    if process.returncode: row["errors"].append(mode+f": returncode={process.returncode}")
                    try: row["errors"] += [mode+": "+error for error in compare(math_raw, stdout, case)]
                    except ValueError as error: row["errors"].append(mode+": "+str(error))
                    row["modes"][mode]=entry
                    if row["errors"]: break
                if args.matrix == "cache8" and not row["errors"]:
                    row["cache_pairs"] = {}
                    for baseline in MODES:
                        off = row["modes"][baseline+"_cache0"]["raw"]
                        on = row["modes"][baseline+"_cache1"]["raw"]
                        equal = off["size"] == on["size"] == d.SNAPSHOT_SIZE and off["sha256"] == on["sha256"]
                        row["cache_pairs"][baseline] = {"raw_equal":equal,"snapshot_sha256":off["sha256"]}
                        if not equal: row["errors"].append(baseline+": cache OFF/ON raw snapshots differ")
            except Exception as error: row["errors"].append(type(error).__name__+": "+str(error))
            row["completed_utc"]=utc(); journal.write(json.dumps(row, separators=(",", ":"))+"\n")
            report["failed_cases" if row["errors"] else "passed_cases"]+=1
            report["function_statistics"]={mode:dict(values) for mode,values in aggregates.items()}
            report["x64_emission_totals"]={mode:dict(values) for mode,values in emission_totals.items()}
            report["flags_liveness_policy"] = args.flags_liveness
            report["flags_liveness_totals"] = {mode:dict(values) for mode,values in flags_totals.items()}
            report["straight_flags_liveness_totals"] = {mode:dict(values) for mode,values in straight_flags_totals.items()}
            report["straight_pressure_emission_totals"]={mode:dict(values) for mode,values in straight_emission_totals.items()}
            if row["errors"]: report["first_failure"]=row; (output/"failures"/(case.name+".elf")).write_bytes(elf)
            if row["errors"] or (index+1)%50==0:
                for stream in (*streams.values(), journal): stream.flush(); os.fsync(stream.fileno())
                report["last_checkpoint_utc"]=utc(); checkpoint(report_path, report)
                print(f"cases={index+1}/{len(cases)} processes={report['executed']} failed={report['failed_cases']}", flush=True)
            if row["errors"]: raise ValueError(f"{case.name}: {row['errors'][:5]}")
        for stream in (*streams.values(), journal): stream.flush(); os.fsync(stream.fileno())
        report["source_sha256_completion"]=sources(); report["runner"]["sha256_completion"]=file_sha(pinned)
        report["runner"]["path_sha256_completion"]=file_sha(runner)
        if sources()!=startup or file_sha(pinned)!=args.runner_sha256 or file_sha(runner)!=args.runner_sha256: raise ValueError("frozen files changed")
        if report["executed"]!=report["planned_processes"] or len(process_ids)!=report["executed"]: raise ValueError("execution/PID accounting mismatch")
        if any(report[k]!=len(cases) for k in ("attempted", "math_checked", "unicorn_executed", "passed_cases")): raise ValueError("case accounting mismatch")
        if args.matrix == "cache8" and args.cache_stress:
            enabled_straight = [straight_emission_totals[mode] for mode, env in modes.items()
                                if env["SVM_X64_SCALAR_CACHE"] == "1"]
            if not any(values["scalar_cache_reads"] > 0 and values["scalar_cache_writes"] > 0
                       for values in enabled_straight):
                raise ValueError("cache ON straight pressure did not emit nonzero read/write sites")
            report["cache_pressure_emission_proven"] = True
            report["cache_evidence_boundary"] = "same-runner raw math/Unicorn comparison plus explicit cache policy environments and static read/write emission sites; no dynamic hit-count claim"
        if args.require_coalescing:
            if not any(values["coalesced_binary"] > 0 for mode, values in straight_emission_totals.items()
                       if modes[mode]["SVM_X64_SCALAR_CACHE"] == "1"):
                raise ValueError("cache ON straight programs emitted no coalesced binary sites")
            report["coalescing_emission_proven"] = True
            report["coalescing_evidence_boundary"] = "static audited cache-operand binary emission sites after Translate; not runtime counts or proof of publication"
        if args.flags_liveness == "1":
            if not any(values["narrowed"] > 0 for values in straight_flags_totals.values()):
                raise ValueError("flags-liveness pass did not narrow a real straight guest program")
            report["flags_liveness_emission_proven"] = True
        report["distinct_process_ids"]=len(process_ids)
        report["artifact_sha256"]={path.name:file_sha(path) for path in output.iterdir() if path.is_file() and path!=report_path}
        report["complete"]=report["success"]=True; report["completed_utc"]=utc(); checkpoint(report_path,report)
        print(f"PASS cases={len(cases)} processes={report['executed']} evidence={output}",flush=True)
    except BaseException as error:
        for stream in (*streams.values(), journal): stream.flush(); os.fsync(stream.fileno())
        report["error"]=type(error).__name__+": "+str(error); report["stopped_utc"]=utc(); checkpoint(report_path, report); raise
    finally:
        for stream in (*streams.values(),journal): stream.close()
        os.close(fd)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-only",action="store_true"); parser.add_argument("--runner",type=Path)
    parser.add_argument("--runner-sha256"); parser.add_argument("--runner-arg",action="append",default=[])
    parser.add_argument("--output",type=Path); parser.add_argument("--timeout",type=float,default=10)
    parser.add_argument("--matrix",choices=("cfg4","cache8"),default="cfg4")
    parser.add_argument("--cache-stress",action="store_true",help="append straight-line scalar pressure/flags/returning-helper cases")
    parser.add_argument("--require-coalescing",action="store_true",help="require static cache-operand binary sites in the cache8 straight-program matrix")
    parser.add_argument("--flags-liveness",choices=("0", "1"),default="0",help="pin the x64 local carry-chain publication policy; ON requires actual straight-guest narrowing")
    args=parser.parse_args()
    if args.require_coalescing and (args.matrix != "cache8" or not args.cache_stress or args.validate_only):
        parser.error("--require-coalescing needs an executing --matrix cache8 --cache-stress run")
    if args.flags_liveness == "1" and (args.matrix != "cache8" or not args.cache_stress or args.validate_only):
        parser.error("--flags-liveness 1 needs an executing --matrix cache8 --cache-stress run")
    cases=corpus()
    if args.cache_stress: cases += cache_stress_corpus()
    if args.validate_only:
        for case, expected, parameters in cases:
            errors=compare(expected,execute_elf(case,d.make_elf(case)),case)
            if errors: raise ValueError(case.name+": "+str(errors))
        case, raw, _=cases[0]; corrupt=raw[:272]+bytes([raw[272]^1])+raw[273:]
        if not compare(raw,corrupt,case): raise ValueError("corruption accepted")
        print(f"PASS {len(cases)} complete math/Unicorn snapshots; SwiftVM executed=0"); return
    if not args.runner or not args.output or not re.fullmatch("[0-9a-f]{64}",args.runner_sha256 or "") or args.timeout<=0:
        parser.error("requires immutable --runner, --runner-sha256, ext4 --output and positive timeout")
    run(args,cases)

if __name__=="__main__":
    try: main()
    except (OSError,ValueError,RuntimeError) as error:
        print("ERROR: "+str(error),file=sys.stderr); raise SystemExit(1)
