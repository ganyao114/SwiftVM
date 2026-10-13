#!/usr/bin/env python3
"""Real A64 ELF / Unicorn / native Core Step comparison, cache OFF and ON.

Run in Linux/WSL against an immutable x64 runner with SVM_ARM64_STEP support.
The 336-byte protocol exports X0-X30, NZCV and 64 memory bytes; it is not a
complete ThreadContext64 protocol. No ordinary Run fallback is accepted.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import stat
import struct
import subprocess
import sys
from collections import Counter

import arm64_differential as d
import arm64_verify_cfg as cfg
import arm64_verify_local_cfg_cache as local
import arm64_fp_corpus as fp
import arm64_half_corpus as half
import arm64_simd_corpus as simd
import arm64_simd_math as simd_math

SCOPE = "a64_runtime_core_step_cache2"
MODES = {f"cache{cache}": {
    "SVM_ARM64_STEP": "1", "SVM_ENABLE_JIT": "1",
    "SVM_X64_SCALAR_CACHE": str(cache), "SVM_FUNC_BASE": "0",
    "SVM_UNIFORM_ELIM": "0", "SVM_FLAG_FULL_ELIM": "0",
    "SVM_MEM_DIRECT": "0", "SVM_GUEST_BITS": "32",
} for cache in (0, 1)}
PROOF = re.compile(rb"\[arm64-step\] calls=(\d+) retired=(\d+) syscalls=(\d+) run_calls=(\d+)")


def dependencies():
    modules = (d, cfg, local, fp, half, simd, simd_math)
    paths = [Path(__file__).resolve(), *(Path(module.__file__).resolve() for module in modules)]
    return {str(path): cfg.file_sha(path) for path in paths}


def sample(items, count):
    if len(items) < count:
        raise ValueError(f"scope shrank: need {count}, have {len(items)}")
    return [items[index * len(items) // count] for index in range(count)]


def initial(case):
    memory = case.memory if case.memory is not None else bytes((0x80 + 17 * i) & 255 for i in range(64))
    return [case.seeds.get(i, 0x100 + i) for i in range(29)] + [d.MEMORY_BASE, d.DATA_BASE], bytearray(memory), case.nzcv


def raw(case, regs, memory, flags):
    value = d.MAGIC + struct.pack("<Q31QQ", d.case_id(case), *regs, flags) + bytes(memory)
    d.decode_snapshot(value, case)
    return value


def scalar_lse():
    """Reuse the scalar CAS/LSE encodings and operands from the existing corpus."""
    result = []
    for bits in (8, 16, 32, 64):
        size, mask = {8: 0, 16: 1, 32: 2, 64: 3}[bits], (1 << bits) - 1
        for operation, opcode in (("ldadd", 0), ("ldclr", 1), ("ldeor", 2), ("ldset", 3), ("swp", 8)):
            case = d.Case(f"step_lse_{operation}_{bits}", (0x382003A1 | (size << 30) | (opcode << 12),),
                          f"{operation} x0 operand / x1 old / [x29]", {0: 0x0123456789ABCDEF, 1: d.MASK64}, 0xB0000000, "scalar_lse")
            regs, memory, flags = initial(case)
            old, operand = int.from_bytes(memory[:bits // 8], "little"), regs[0] & mask
            updated = {"ldadd": old + operand, "ldclr": old & ~operand,
                       "ldeor": old ^ operand, "ldset": old | operand, "swp": operand}[operation] & mask
            regs[1] = old
            memory[:bits // 8] = updated.to_bytes(bits // 8, "little")
            result.append((case, raw(case, regs, memory, flags), {"family": "scalar_lse", "operation": operation, "width": bits}))
    for bits in (32, 64):
        for match in (False, True):
            template = d.Case("unused", (), "")
            _, memory, _ = initial(template)
            old = int.from_bytes(memory[:bits // 8], "little")
            case = d.Case(f"step_cas_{bits}_{int(match)}", (0x08A07FA1 | ({32: 2, 64: 3}[bits] << 30),),
                          "CAS x0 expected / x1 desired / [x29]", {0: old if match else old ^ 1, 1: 0x0011223344556677}, 0xB0000000, "scalar_lse")
            regs, memory, flags = initial(case)
            regs[0] = old
            if match:
                memory[:bits // 8] = (regs[1] & ((1 << bits) - 1)).to_bytes(bits // 8, "little")
            result.append((case, raw(case, regs, memory, flags), {"family": "scalar_lse", "operation": "cas", "width": bits, "match": match}))
    return result


def pc_relative():
    result = []
    for page in (False, True):
        for delta in (-3, -1, 1, 5):
            field = delta & 0x1FFFFF
            case = d.Case(f"step_{'adrp' if page else 'adr'}_{delta}",
                          ((0x90000000 if page else 0x10000000) | ((field & 3) << 29) | ((field >> 2) << 5) | 2,),
                          f"{'adrp' if page else 'adr'} x2,{delta}", family="pc_relative")
            regs, memory, flags = initial(case)
            # Locate the instruction after make_elf's variable MOV prologue.
            prologue = sum(len(d.mov64(i, case.seeds.get(i, 0x100 + i))) for i in range(29))
            prologue += len(d.mov64(29, d.MEMORY_BASE)) + len(d.mov64(30, d.DATA_BASE))
            prologue += len(d.mov64(28, case.nzcv)) + 1 + len(d.mov64(28, case.seeds.get(28, 0x11C)))
            pc = d.CODE_BASE + 4096 + prologue * 4
            regs[2] = ((pc & ~4095) + delta * 4096 if page else pc + delta) & d.MASK64
            result.append((case, raw(case, regs, memory, flags), {"family": "pc_relative", "page": page, "delta": delta, "pc": pc}))
    return result


def corpus():
    result = [(case, expected, {**params, "family": "ccmp_ccmn"})
              for case, expected, params in local.corpus() if params["profile"] in (0, 1)]
    groups = {}
    for entry in cfg.corpus():
        groups.setdefault(entry[2]["family"], []).append(entry)
    for family in sorted(groups):
        result.extend(sample(groups[family], 12 if family in ("cbz", "cbnz", "flags", "loop") else 8))
    result += pc_relative() + scalar_lse()
    result += [(case, None, {"family": "scalar_fp", "math": "partial architectural fields"}) for case in fp.corpus(d.Case)]
    result += [(case, None, {"family": "fp16", "math": "partial architectural fields"}) for case in sample(half.corpus(d.Case), 40)]
    result += [(case, None, {"family": "simd_helper", "math": "64 memory bytes and NZCV"}) for case in sample(simd.corpus(d.Case), 64)]
    if not 400 <= len(result) <= 500 or len({case.name for case, _, _ in result}) != len(result):
        raise ValueError("runtime Step corpus size or uniqueness changed")
    return result


def partial_math(case, observed):
    state = d.decode_snapshot(observed, case)
    regs = {name: int(value, 16) for name, value in state["registers"].items()}
    memory, flags = bytes.fromhex(state["memory"]), int(state["nzcv"], 16)
    if case.family == "half":
        wanted = half.expected(case)
        ok = memory.startswith(wanted["memory"]) and flags == wanted["nzcv"]
        ok &= regs["x27"] == wanted["fpsr"] and regs["x28"] == wanted["fpcr"]
        ok &= "x0" not in wanted or regs["x0"] == wanted["x0"]
        return [] if ok else ["FP16 mathematical architectural fields disagree"]
    if case.family == "simd":
        expected = simd_math.expected_memory(case)
        if expected is None or len(expected) != 64:
            return ["SIMD fixture lacks its independent 64-byte mathematical expectation"]
        return [] if memory == expected and flags == case.nzcv else ["SIMD mathematical memory/NZCV disagree"]
    # Existing scalar FP result-bit/status expectations from arm64_verify_corpus.
    expected = {
        "fp_div_by_zero": (0x7F800000, 2, 4), "fp_sqrt_negative": (0x7FC00000, 1, 4),
        "fp_add_snan": (0x7FC00123, 1, 4), "fp_add_snan_dn": (0x7FC00000, 1, 4),
        "fp_add_input_denormal_fz": (0x3F800000, 128, 4), "fp_div_output_denormal_fz": (0, 8, 4),
        "fp_frintn_half": (0x40000000, 0, 4), "fp_frinta_half": (0x40400000, 0, 4),
        "fp_frintx_half": (0x40000000, 16, 4), "fp_cvt_s_to_d": (0x4004000000000000, 0, 8),
        "fp_scvtf_negative": (0xBFF0000000000000, 0, 8), "fp_fma_fused": (0xA8800000, 0, 4),
        "fp_min_zero": (0x80000000, 0, 4), "fp_max_zero": (0, 0, 4),
        "fp_div_denormal_gradual": (0x00400000, 0, 4), "fp_double_div": (0x3FD5555555555555, 16, 8),
        "fp_fnmul_qnan": (0xFFC00123, 1, 4), "fp_fnmul_inf_zero": (0xFFC00000, 1, 4),
        "fp_fmadd_inf_zero_qnan": (0x7FC00000, 1, 4), "fp_frinti_negative_zero": (0x80000000, 0, 4),
        "fp_fz_tiny_inexact": (0, 8, 4),
    }
    for mode in range(4):
        expected[f"fp_add_round_{mode}"] = (0x3F800001 if mode == 1 else 0x3F800000, 16, 4)
        expected[f"fp_add_cancel_{mode}"] = (0x80000000 if mode == 2 else 0, 0, 4)
        expected[f"fp_mul_overflow_{mode}"] = (0x7F800000 if mode in (0, 1) else 0x7F7FFFFF, 20, 4)
    if case.name in expected:
        bits, fpsr, size = expected[case.name]
        ok = memory.startswith(bits.to_bytes(size, "little").ljust(16, b"\0"))
        ok &= regs["x27"] == fpsr and regs["x28"] == case.seeds[27] and flags == case.nzcv
    else:
        comparisons = {"fp_cmp_qnan": (0, 0x30000000), "fp_cmpe_qnan": (1, 0x30000000),
                       "fp_cvtzs_nan": (1, 0xB0000000), "fp_fccmp_false": (0, 0),
                       "fp_fccmp_true": (0, 0x60000000), "fp_fccmpe_false_nan": (0, 0),
                       "fp_fccmpe_true_nan": (1, 0x30000000)}
        if case.name not in comparisons:
            return ["scalar FP fixture has no independent expectation"]
        fpsr, nzcv = comparisons[case.name]
        ok = regs["x27"] == fpsr and flags == nzcv and regs["x28"] == case.seeds[27]
        ok &= case.name != "fp_cvtzs_nan" or regs["x0"] == 0
    return [] if ok else ["scalar FP mathematical architectural fields disagree"]


class Oracle:
    def __init__(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM, __version__ as cs_version
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR, UC_HOOK_CODE, __version__ as uc_version
        import unicorn.arm64_const as regs
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.Uc, self.arch, self.mode = Uc, UC_ARCH_ARM64, UC_MODE_ARM
        self.intr, self.code, self.r = UC_HOOK_INTR, UC_HOOK_CODE, regs
        self.versions = {"unicorn": uc_version, "capstone": cs_version, "cpu_model": "UC_CPU_ARM64_MAX"}

    def execute(self, case, elf):
        code, data = (struct.unpack_from("<IIQQQQQQ", elf, offset) for offset in (64, 120))
        if len(list(self.decoder.disasm(elf[4096:code[5]], d.CODE_BASE + 4096))) * 4 != code[5] - 4096:
            raise ValueError("Capstone rejected ELF instructions")
        cpu = self.Uc(self.arch, self.mode); cpu.ctl_set_cpu_model(self.r.UC_CPU_ARM64_MAX)
        cpu.mem_map(d.CODE_BASE, 65536); cpu.mem_map(d.DATA_BASE, 65536)
        cpu.mem_write(d.CODE_BASE, elf[:code[5]]); cpu.mem_write(d.DATA_BASE, elf[data[2]:data[2] + data[5]])
        snapshots, exits, counts = [], [], {"instructions": 0, "syscalls": 0}
        def instruction(emulator, address, size, unused):
            counts["instructions"] += 1
            if size != 4:
                raise ValueError("non-A64 instruction width")
        def interrupt(emulator, number, unused):
            counts["syscalls"] += 1
            syscall = emulator.reg_read(self.r.UC_ARM64_REG_X8)
            if syscall == 64:
                if emulator.reg_read(self.r.UC_ARM64_REG_X0) != 1 or emulator.reg_read(self.r.UC_ARM64_REG_X2) != d.SNAPSHOT_SIZE:
                    raise ValueError("invalid stdout snapshot write")
                snapshots.append(bytes(emulator.mem_read(emulator.reg_read(self.r.UC_ARM64_REG_X1), d.SNAPSHOT_SIZE)))
                emulator.reg_write(self.r.UC_ARM64_REG_X0, d.SNAPSHOT_SIZE)
            elif syscall == 93:
                if emulator.reg_read(self.r.UC_ARM64_REG_X0):
                    raise ValueError("harness error exit")
                exits.append(True); emulator.emu_stop()
            else:
                raise ValueError(f"unexpected interrupt={number}/syscall={syscall}")
        cpu.hook_add(self.code, instruction); cpu.hook_add(self.intr, interrupt)
        cpu.emu_start(d.CODE_BASE + 4096, 0, count=10000)
        if len(snapshots) != 1 or len(exits) != 1 or counts["syscalls"] != 2:
            raise ValueError("Unicorn snapshot/exit accounting mismatch")
        d.decode_snapshot(snapshots[0], case)
        return snapshots[0], counts


def step_proof(stderr, counts):
    matches = PROOF.findall(stderr)
    if len(matches) != 1:
        raise ValueError("expected exactly one Core Step execution proof")
    calls, retired, syscalls, run_calls = map(int, matches[0])
    if run_calls or syscalls != 2 or calls != retired + syscalls or not retired:
        raise ValueError("Core Step proof has fallback, missing retirement, or bad call accounting")
    if calls != counts["instructions"] or syscalls != counts["syscalls"]:
        raise ValueError("Core Step calls disagree with independent Unicorn dynamic instruction count")
    return dict(calls=calls, retired=retired, syscalls=syscalls, run_calls=run_calls)


def observe_process(process, command, identity):
    observation = {}
    proc = Path(f"/proc/{process.pid}")
    try:
        observed = (proc / "exe").stat()
        observation.update(proc_exe=os.readlink(proc / "exe"), proc_exe_device=observed.st_dev, proc_exe_inode=observed.st_ino)
        if (observed.st_dev, observed.st_ino) != identity:
            raise ValueError("child executable identity mismatch")
        raw_argv = (proc / "cmdline").read_bytes()
        argv = raw_argv.decode("utf-8", "replace").split("\0")
        if argv and argv[-1] == "":
            argv.pop()
        observation["proc_cmdline"] = argv
        if raw_argv:
            if argv != command:
                raise ValueError("child argv mismatch")
        else:
            observation["proc_cmdline_unavailable"] = "process became terminal after executable observation"
    except (FileNotFoundError, ProcessLookupError):
        observation["proc_inspection_unavailable"] = "process became terminal before inspection completed"
    return observation


def run(args):
    cases, oracle = corpus(), Oracle()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    filesystem = subprocess.check_output(["stat", "-f", "-c", "%T", str(output)], text=True).strip()
    if filesystem not in ("ext2/ext3", "ext4"):
        raise ValueError("evidence output must be on ext4")
    runner = args.runner.resolve(strict=True); runner_stat = runner.stat()
    if not stat.S_ISREG(runner_stat.st_mode) or runner_stat.st_mode & 0o222 or not os.access(runner, os.X_OK):
        raise ValueError("runner must be a read-only regular executable")
    descriptor = os.open(runner, os.O_RDONLY); pinned = f"/proc/self/fd/{descriptor}"
    identity = (runner_stat.st_dev, runner_stat.st_ino)
    if (os.fstat(descriptor).st_dev, os.fstat(descriptor).st_ino) != identity or cfg.file_sha(pinned) != args.runner_sha256:
        os.close(descriptor); raise ValueError("runner SHA256/identity mismatch")
    startup = dependencies()
    report = {"version": 1, "scope": SCOPE, "complete": False, "success": False, "started_utc": cfg.utc(),
              "planned_cases": len(cases), "planned_processes": len(cases) * len(MODES), "modes": MODES,
              "attempted": 0, "unicorn_executed": 0, "math_checked": 0, "full_math_checked": 0,
              "partial_math_checked": 0, "executed": 0, "passed_cases": 0, "failed_cases": 0,
              "families": dict(Counter(params["family"] for _, _, params in cases)), "oracle": oracle.versions,
              "filesystem": filesystem, "source_sha256_startup": startup,
              "runner": {"path": str(runner), "sha256_startup": args.runner_sha256, "device": identity[0], "inode": identity[1]},
              "process_evidence": {"pid": os.getpid(), "argv": sys.argv, "python": sys.version, "platform": platform.platform()},
              "evidence_boundary": "Actual ARM64 ELF execution uses only native Core Step, verified by driver call counts against Unicorn dynamic instruction counts. Complete raw 336-byte comparisons cover X0-X30, NZCV and 64 memory bytes. Selected FP/FP16 fixtures capture FPSR/FPCR as X27/X28 and vector results in memory. They do not export full PC/SP/Q0-Q31/pstate/padding/tpidr or full ThreadContext64. Math is full snapshot for CCMP/CFG/ADR/LSE and partial declared fields for FP/FP16/SIMD. No Android, timing, fault-recovery, cache-hit, function/optimization-pass, or source-to-build identity claim.",
              "ordinary_run_evidence_used": False, "source_to_build_identity_claim": False}
    path = output / "report.json"; cfg.checkpoint(path, report)
    (output / "elves").mkdir()
    streams = {name: (output / f"{name}.bin").open("wb") for name in ("math", "unicorn", *MODES, "stderr")}
    journal = (output / "cases.jsonl").open("w", encoding="utf-8")
    def append(name, value):
        stream = streams[name]; reference = {"offset": stream.tell(), "size": len(value), "sha256": cfg.sha(value)}
        stream.write(value); return reference
    def flush():
        for stream in (*streams.values(), journal):
            stream.flush(); os.fsync(stream.fileno())
    try:
        for index, (case, expected, params) in enumerate(cases):
            report["attempted"] += 1; elf = d.make_elf(case)
            elf_path = output / "elves" / f"{case.name}.elf"; elf_path.write_bytes(elf)
            row = {"index": index, "name": case.name, "parameters": params, "elf_sha256": cfg.sha(elf),
                   "elf_path": str(elf_path.relative_to(output)), "errors": [], "modes": {}}
            try:
                uc_raw, counts = oracle.execute(case, elf)
                report["unicorn_executed"] += 1; row["unicorn"] = append("unicorn", uc_raw); row["unicorn_dynamic"] = counts
                if expected is not None:
                    row["math"] = append("math", expected); row["errors"] += cfg.compare(expected, uc_raw, case)
                    report["full_math_checked"] += 1
                else:
                    row["errors"] += partial_math(case, uc_raw); report["partial_math_checked"] += 1
                report["math_checked"] += 1
                if row["errors"]:
                    raise ValueError("Unicorn/math disagreement")
                for mode, overrides in MODES.items():
                    environment = {key: value for key, value in os.environ.items() if not key.startswith(("SVM_", "SWIFT_"))}
                    environment.update(overrides); command = [str(runner), *args.runner_arg, str(elf_path)]
                    process = subprocess.Popen(command, executable=pinned, pass_fds=(descriptor,), env=environment,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    report["executed"] += 1
                    entry = {"pid": process.pid, "command": command, "environment": overrides,
                             "pinned_executable": pinned, "device": identity[0], "inode": identity[1],
                             "started_utc": cfg.utc(), "timed_out": False}
                    row["modes"][mode] = entry
                    try:
                        entry.update(observe_process(process, command, identity))
                        stdout, stderr = process.communicate(timeout=args.timeout)
                    except subprocess.TimeoutExpired:
                        entry["timed_out"] = True; process.kill(); stdout, stderr = process.communicate()
                        row["errors"].append(mode + ": timeout")
                    except BaseException:
                        process.kill(); stdout, stderr = process.communicate()
                        entry.update(returncode=process.returncode, wait_completed=True, raw=append(mode, stdout), stderr=append("stderr", stderr))
                        raise
                    entry.update(returncode=process.returncode, wait_completed=True, completed_utc=cfg.utc(),
                                 raw=append(mode, stdout), stderr=append("stderr", stderr))
                    try:
                        if process.returncode:
                            raise ValueError(f"runner exit={process.returncode}; failed Step is not a pass")
                        entry["step_proof"] = step_proof(stderr, counts)
                        row["errors"] += [mode + ": " + error for error in cfg.compare(uc_raw, stdout, case)]
                        row["errors"] += [mode + ": " + error for error in (cfg.compare(expected, stdout, case) if expected is not None else partial_math(case, stdout))]
                    except ValueError as error:
                        row["errors"].append(mode + ": " + str(error))
                if len(row["modes"]) == 2:
                    row["cache_pair_raw_equal"] = row["modes"]["cache0"]["raw"]["sha256"] == row["modes"]["cache1"]["raw"]["sha256"]
                    if not row["cache_pair_raw_equal"]:
                        row["errors"].append("cache policy raw snapshots disagree")
            except Exception as error:
                row["errors"].append(type(error).__name__ + ": " + str(error))
            report["failed_cases" if row["errors"] else "passed_cases"] += 1
            journal.write(json.dumps(row, sort_keys=True) + "\n")
            if row["errors"] or (index + 1) % 20 == 0:
                flush(); cfg.checkpoint(path, report)
            print(f"{'FAIL' if row['errors'] else 'PASS'} {index + 1}/{len(cases)} {case.name}", flush=True)
        report["source_sha256_completion"] = dependencies()
        report["runner"]["sha256_completion"] = cfg.file_sha(pinned)
        report["runner"]["path_sha256_completion"] = cfg.file_sha(runner)
        final_stat = runner.stat()
        if dependencies() != startup or cfg.file_sha(pinned) != args.runner_sha256 or cfg.file_sha(runner) != args.runner_sha256 or (final_stat.st_dev, final_stat.st_ino) != identity:
            raise ValueError("frozen runner or tool dependency changed")
        report["complete"] = report["attempted"] == len(cases)
        report["success"] = report["complete"] and report["passed_cases"] == len(cases) and report["executed"] == len(cases) * len(MODES) and report["unicorn_executed"] == len(cases) and report["math_checked"] == len(cases)
        report["completed_utc"] = cfg.utc(); cfg.checkpoint(path, report)
        if not report["success"]:
            raise ValueError(f"Step comparison failed: passed={report['passed_cases']} failed={report['failed_cases']}")
    except BaseException as error:
        report["error"] = type(error).__name__ + ": " + str(error); cfg.checkpoint(path, report); raise
    finally:
        flush()
        for stream in (*streams.values(), journal): stream.close()
        os.close(descriptor)
        files = {str(item.relative_to(output)): {"size": item.stat().st_size, "sha256": cfg.file_sha(item)}
                 for item in sorted(output.rglob("*")) if item.is_file() and item.name != "manifest.json"}
        cfg.checkpoint(output / "manifest.json", {"version": 1, "scope": SCOPE, "files": files, "self_hash_boundary": "manifest excludes itself"})
    print(f"PASS cases={len(cases)} native Step processes={report['executed']} evidence={output}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--runner-sha256")
    parser.add_argument("--runner-arg", action="append", default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--inventory-only", action="store_true", help="count fixtures; zero Unicorn or SwiftVM execution")
    args = parser.parse_args()
    if args.inventory_only:
        cases = corpus(); print(json.dumps({"scope": SCOPE, "cases": len(cases), "processes": len(cases) * 2,
                                          "families": dict(Counter(params["family"] for _, _, params in cases)), "SwiftVM_executed": 0}, indent=2)); return
    if not args.runner or not args.output or not re.fullmatch("[0-9a-f]{64}", args.runner_sha256 or "") or args.timeout <= 0:
        parser.error("requires --runner, --runner-sha256, new ext4 --output and positive --timeout")
    run(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print("ERROR: " + str(error), file=sys.stderr); raise SystemExit(1)
