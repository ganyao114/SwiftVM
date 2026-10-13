#!/usr/bin/env python3
"""Compare an actual LoongArch SwiftVM runner under QEMU with Unicorn ARM64.

This uses the project's existing complete ELF snapshot protocol. No ARM64
hardware or adb is required. QEMU executes the LoongArch JIT; Unicorn executes
the original ARM64 guest ELF. LBT is not part of this simulator qualification.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

TASK_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TASK_ROOT / "tools"))
import arm64_differential as d

STEP_PROOF = re.compile(rb"\[arm64-step\] calls=(\d+) retired=(\d+) syscalls=(\d+) run_calls=(\d+)")


def sha_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checkpoint(path: Path, report: dict) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


class Oracle:
    def __init__(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM, __version__ as cs_version
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR, UC_HOOK_CODE, __version__ as uc_version
        import unicorn.arm64_const as registers
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.Uc, self.arch, self.mode, self.interrupt = Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
        self.code_hook = UC_HOOK_CODE
        self.regs = registers
        self.versions = {"capstone": cs_version, "unicorn": uc_version, "cpu": "UC_CPU_ARM64_MAX"}

    def execute(self, case, elf: bytes, count_instructions: bool = False) -> tuple[bytes, dict]:
        code = struct.unpack_from("<IIQQQQQQ", elf, 64)
        data = struct.unpack_from("<IIQQQQQQ", elf, 120)
        instructions = list(self.decoder.disasm(elf[4096:code[5]], d.CODE_BASE + 4096))
        if len(instructions) * 4 != code[5] - 4096:
            raise ValueError(f"{case.name}: Capstone rejected the ARM64 ELF harness")
        cpu = self.Uc(self.arch, self.mode)
        cpu.ctl_set_cpu_model(self.regs.UC_CPU_ARM64_MAX)
        cpu.mem_map(d.CODE_BASE, 65536)
        cpu.mem_map(d.DATA_BASE, 65536)
        cpu.mem_write(d.CODE_BASE, elf[:code[5]])
        cpu.mem_write(d.DATA_BASE, elf[data[2]:data[2] + data[5]])
        snapshots, exits = [], []
        counts = {"instructions": 0, "syscalls": 0}

        def instruction(_emulator, _address, _size, _context):
            counts["instructions"] += 1

        def interrupt(emulator, number, _):
            counts["syscalls"] += 1
            r = self.regs
            syscall = emulator.reg_read(r.UC_ARM64_REG_X8)
            if syscall == 64:
                if emulator.reg_read(r.UC_ARM64_REG_X0) != 1 or emulator.reg_read(r.UC_ARM64_REG_X2) != d.SNAPSHOT_SIZE:
                    raise ValueError(f"{case.name}: invalid snapshot syscall")
                snapshots.append(bytes(emulator.mem_read(emulator.reg_read(r.UC_ARM64_REG_X1), d.SNAPSHOT_SIZE)))
                emulator.reg_write(r.UC_ARM64_REG_X0, d.SNAPSHOT_SIZE)
            elif syscall == 93:
                if emulator.reg_read(r.UC_ARM64_REG_X0) != 0:
                    raise ValueError(f"{case.name}: ARM64 harness error exit")
                exits.append(True)
                emulator.emu_stop()
            else:
                raise ValueError(f"{case.name}: unexpected interrupt={number}, syscall={syscall}")

        cpu.hook_add(self.interrupt, interrupt)
        if count_instructions:
            cpu.hook_add(self.code_hook, instruction)
        cpu.emu_start(d.CODE_BASE + 4096, 0, count=4000)
        if len(snapshots) != 1 or len(exits) != 1:
            raise ValueError(f"{case.name}: missing or duplicated ARM64 snapshot/exit")
        d.decode_snapshot(snapshots[0], case)
        return snapshots[0], counts


def step_proof(stderr: bytes, counts: dict) -> dict:
    matches = STEP_PROOF.findall(stderr)
    if len(matches) != 1:
        raise ValueError("expected exactly one Core Step execution proof")
    calls, retired, syscalls, run_calls = map(int, matches[0])
    if run_calls or syscalls != 2 or calls != retired + syscalls or not retired:
        raise ValueError("Core Step proof has fallback, missing retirement, or bad call accounting")
    if calls != counts["instructions"] or syscalls != counts["syscalls"]:
        raise ValueError("Core Step calls disagree with independent Unicorn instruction count")
    return dict(calls=calls, retired=retired, syscalls=syscalls, run_calls=run_calls)


def run(args) -> int:
    runner = args.runner.resolve(strict=True)
    qemu = Path(shutil.which(args.qemu) or args.qemu).resolve(strict=True)
    simulator = {"path": str(qemu), "sha256": sha_file(qemu),
                 "version": subprocess.check_output([str(qemu), "--version"], text=True).strip()}
    with runner.open("rb") as stream:
        header = stream.read(64)
    if header[:4] != b"\x7fELF" or len(header) < 20 or struct.unpack_from("<H", header, 18)[0] != 258:
        raise ValueError("--runner must be a LoongArch ELF executable; no SwiftVM cases executed")
    cases = d.corpus()
    if args.group != "all":
        cases = [case for case in cases if case.family == args.group]
    available = {}
    for case in cases:
        available[case.family] = available.get(case.family, 0) + 1
    if args.case:
        unknown = set(args.case) - {case.name for case in cases}
        if unknown:
            raise ValueError(f"unknown selected case(s): {sorted(unknown)}")
        cases = [case for case in cases if case.name in args.case]
    if args.sample_per_family is not None:
        if args.sample_per_family <= 0:
            raise ValueError("--sample-per-family must be positive")
        families = {}
        for case in cases:
            families.setdefault(case.family, []).append(case)
        cases = [case for family in families.values()
                 for case in sorted(family, key=lambda item: (hashlib.sha256(item.name.encode()).digest(), item.name))[:args.sample_per_family]]
    if not cases:
        raise ValueError("zero selected cases")
    selected = {}
    for case in cases:
        selected[case.family] = selected.get(case.family, 0) + 1
    coverage = {name: {"available": count, "selected": selected.get(name, 0)} for name, count in available.items()}
    for name, count in coverage.items():
        print(f"FAMILY {name}: selected={count['selected']} available={count['available']}", flush=True)
    environment = os.environ.copy()
    overrides = {"SVM_ENABLE_JIT": "1", "SVM_JIT_CACHE": "", "SVM_ARM64_STEP": "1" if args.step else "0"}
    for value in args.env:
        name, separator, contents = value.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or "\0" in contents:
            raise ValueError(f"invalid --env assignment: {value!r}")
        overrides[name] = contents
    if overrides["SVM_ENABLE_JIT"] != "1":
        raise ValueError("LoongArch qualification requires SVM_ENABLE_JIT=1")
    if overrides["SVM_ARM64_STEP"] != ("1" if args.step else "0"):
        raise ValueError("select --step to enable Core Step with independent retirement proof")
    environment.update(overrides)
    oracle = Oracle()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source_runner = runner
    source_digest = sha_file(source_runner)
    runner = output / "runner.loongarch"
    shutil.copy2(source_runner, runner)
    if sha_file(runner) != source_digest:
        raise ValueError("runner changed while freezing the qualification executable")
    report_path = output / "report.json"
    report = {"version": 2, "scope": "arm64-guest-loongarch-qemu", "complete": False,
              "execution_engine": "core-step" if args.step else "core-run",
              "started": datetime.now(timezone.utc).isoformat(), "runner": str(runner),
              "source_runner": str(source_runner), "runner_sha256": source_digest, "qemu": args.qemu, "cpu": args.cpu,
              "simulator": simulator,
              "reference": oracle.versions, "group": args.group, "mode": args.mode, "environment": overrides,
              "sampling": {"method": "sha256-name" if args.sample_per_family is not None else "none",
                           "per_family": args.sample_per_family, "families": coverage,
                           "full_selected_group": len(cases) == sum(available.values())},
              "planned": len(cases), "executed": 0,
              "passed": 0, "failed": 0, "lbt_executed": False, "cases": []}
    checkpoint(report_path, report)
    command_prefix = [str(qemu), "-L", str(args.sysroot), "-cpu", args.cpu]
    if args.runtime_libdir:
        command_prefix.extend(["-E", f"LD_LIBRARY_PATH={args.runtime_libdir}"])
    for case in cases:
        directory = output / case.name
        directory.mkdir(exist_ok=True)
        elf = d.make_elf(case)
        elf_path = directory / "guest.elf"
        elf_path.write_bytes(elf)
        expected, counts = oracle.execute(case, elf, args.step)
        (directory / "unicorn.bin").write_bytes(expected)
        errors = []
        proof = None
        try:
            result = subprocess.run([*command_prefix, str(runner), *args.runner_arg, str(elf_path)],
                                    capture_output=True, timeout=args.timeout, env=environment)
            (directory / "swiftvm.bin").write_bytes(result.stdout)
            (directory / "swiftvm.stderr").write_bytes(result.stderr)
            report["executed"] += 1
            if result.returncode:
                errors.append(f"LoongArch runner exit={result.returncode}: " + result.stderr.decode("utf-8", "replace")[-1500:])
            else:
                if args.step:
                    proof = step_proof(result.stderr, counts)
                actual = d.decode_snapshot(result.stdout, case)
                errors.extend(d.differences(d.decode_snapshot(expected, case), actual))
                if expected != result.stdout and not errors:
                    errors.append("full raw snapshot bytes differ")
        except subprocess.TimeoutExpired as error:
            report["executed"] += 1
            (directory / "swiftvm.bin").write_bytes(error.stdout or b"")
            (directory / "swiftvm.stderr").write_bytes(error.stderr or b"")
            errors.append(f"LoongArch runner timeout={args.timeout}s")
        except (OSError, ValueError, RuntimeError) as error:
            errors.append(str(error))
        report["failed" if errors else "passed"] += 1
        report["cases"].append({"name": case.name, "family": case.family, "elf_sha256": hashlib.sha256(elf).hexdigest(),
                                "unicorn_sha256": hashlib.sha256(expected).hexdigest(), "differences": errors,
                                "step_proof": proof, "unicorn_counts": counts if args.step else None})
        checkpoint(report_path, report)
        print(f"{'FAIL' if errors else 'PASS'} {case.name}", flush=True)
        for error in errors[:4]:
            print("  " + error, flush=True)
    report["complete"] = report["executed"] == report["planned"]
    report["finished"] = datetime.now(timezone.utc).isoformat()
    checkpoint(report_path, report)
    print(f"LoongArch QEMU mode={args.mode} selected={report['planned']} available={sum(available.values())} "
          f"executed={report['executed']} passed={report['passed']} failed={report['failed']}")
    return 0 if report["complete"] and report["failed"] == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-loongarch64")
    parser.add_argument("--cpu", default="la464")
    parser.add_argument("--sysroot", type=Path, default=Path("/root/swiftvm-loongarch-deps/toolchain/cross-tools/target"))
    parser.add_argument("--runtime-libdir", type=Path, default=Path("/root/swiftvm-loongarch-deps/toolchain/cross-tools/loongarch64-unknown-linux-gnu/lib"))
    parser.add_argument("--group", default="scalar", help="existing ARM64 corpus family, or all")
    parser.add_argument("--step", action="store_true", help="execute Core Step and require retirement proof against Unicorn")
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--sample-per-family", type=int,
                        help="stable SHA256-of-name sample limit per family; report selected and available counts")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="repeatable explicit runner environment override; JIT must remain enabled")
    parser.add_argument("--mode", default="default", help="configuration label recorded with explicit --env overrides")
    parser.add_argument("--runner-arg", action="append", default=[])
    parser.add_argument("--output", type=Path, default=Path("build/loongarch-arm64-unicorn"))
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, ImportError, ValueError, RuntimeError) as error:
        print(f"ERROR: {error}; qualification incomplete", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
