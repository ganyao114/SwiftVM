#!/usr/bin/env python3
"""Compare an actual LoongArch SwiftVM runner under QEMU with Unicorn ARM64.

This supports the project's legacy and complete v2 ELF snapshot protocols. No ARM64
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

    def execute(self, case, elf: bytes, count_instructions: bool = False,
                snapshot_version: int = 1) -> tuple[bytes, dict]:
        code = struct.unpack_from("<IIQQQQQQ", elf, 64)
        data = struct.unpack_from("<IIQQQQQQ", elf, 120)
        instructions = list(self.decoder.disasm(elf[4096:code[5]], d.CODE_BASE + 4096))
        if len(instructions) * 4 != code[5] - 4096:
            raise ValueError(f"{case.name}: Capstone rejected the ARM64 ELF harness")
        cpu = self.Uc(self.arch, self.mode)
        cpu.ctl_set_cpu_model(self.regs.UC_CPU_ARM64_MAX)
        cpu.mem_map(d.CODE_BASE, 65536)
        cpu.mem_map(d.DATA_BASE, max(65536, (data[6] + 65535) & ~65535))
        cpu.mem_write(d.CODE_BASE, elf[:code[5]])
        cpu.mem_write(d.DATA_BASE, elf[data[2]:data[2] + data[5]])
        snapshots, exits, direct_states = [], [], []
        counts = {"instructions": 0, "syscalls": 0}
        boundary = d.snapshot_boundary(case, snapshot_version)
        size = d.snapshot_protocol(snapshot_version)["size"]

        def instruction(emulator, address, _size, _context):
            counts["instructions"] += 1
            if snapshot_version == 2 and address == boundary:
                r = self.regs
                # This hook samples the real pre-instruction architectural
                # state. No harness temporary, memory store, or expected value
                # is used to reconstruct a register's contents.
                sp = emulator.reg_read(r.UC_ARM64_REG_SP)
                try:
                    d.capture_scratch_addresses(sp, data[6])
                except ValueError as error:
                    raise ValueError(f"{case.name}: {error}") from error
                direct_states.append(d.encode_full_snapshot(case,
                    pc=emulator.reg_read(r.UC_ARM64_REG_PC), sp=sp,
                    registers=[emulator.reg_read(getattr(r, f"UC_ARM64_REG_X{i}")) for i in range(31)],
                    vectors=[emulator.reg_read(getattr(r, f"UC_ARM64_REG_Q{i}")) for i in range(32)],
                    nzcv=emulator.reg_read(r.UC_ARM64_REG_NZCV),
                    fpcr=emulator.reg_read(r.UC_ARM64_REG_FPCR), fpsr=emulator.reg_read(r.UC_ARM64_REG_FPSR),
                    memory=bytes(emulator.mem_read(d.MEMORY_BASE, 64))))
            elif snapshot_version == 2 and direct_states:
                captured = direct_states[0]
                if emulator.reg_read(self.regs.UC_ARM64_REG_SP) != struct.unpack_from("<Q", captured, d.FULL_OFFSETS["sp"])[0]:
                    raise ValueError(f"{case.name}: capture modified SP")
                if counts["syscalls"] == 0 and emulator.reg_read(self.regs.UC_ARM64_REG_NZCV) != struct.unpack_from("<Q", captured, d.FULL_OFFSETS["nzcv"])[0]:
                    raise ValueError(f"{case.name}: capture modified NZCV before publishing its snapshot")

        def interrupt(emulator, number, _):
            counts["syscalls"] += 1
            r = self.regs
            syscall = emulator.reg_read(r.UC_ARM64_REG_X8)
            if syscall == 64:
                expected_address = d.FULL_SNAPSHOT_BASE if snapshot_version == 2 else d.DATA_BASE
                if (emulator.reg_read(r.UC_ARM64_REG_X0) != 1 or emulator.reg_read(r.UC_ARM64_REG_X2) != size or
                        emulator.reg_read(r.UC_ARM64_REG_X1) != expected_address):
                    raise ValueError(f"{case.name}: invalid snapshot syscall")
                snapshots.append(bytes(emulator.mem_read(emulator.reg_read(r.UC_ARM64_REG_X1), size)))
                emulator.reg_write(r.UC_ARM64_REG_X0, size)
            elif syscall == 93:
                if emulator.reg_read(r.UC_ARM64_REG_X0) != 0:
                    raise ValueError(f"{case.name}: ARM64 harness error exit")
                exits.append(True)
                emulator.emu_stop()
            else:
                raise ValueError(f"{case.name}: unexpected interrupt={number}, syscall={syscall}")

        cpu.hook_add(self.interrupt, interrupt)
        if count_instructions or snapshot_version == 2:
            cpu.hook_add(self.code_hook, instruction)
        cpu.emu_start(d.CODE_BASE + 4096, 0, count=4000)
        if len(snapshots) != 1 or len(exits) != 1:
            raise ValueError(f"{case.name}: missing or duplicated ARM64 snapshot/exit")
        d.decode_snapshot(snapshots[0], case, snapshot_version)
        if snapshot_version == 2:
            if len(direct_states) != 1:
                raise ValueError(f"{case.name}: expected exactly one pre-capture architectural hook")
            if snapshots[0] != direct_states[0]:
                errors = d.differences(d.decode_snapshot(direct_states[0], case, 2),
                                       d.decode_snapshot(snapshots[0], case, 2))
                raise ValueError(f"{case.name}: guest harness polluted its sampled state: {errors}")
            counts["state_captures"] = len(direct_states)
            # Preserve the observed PC in evidence, rather than substituting
            # the expected harness location used to select the code hook.
            counts["capture_pc"] = d.decode_snapshot(direct_states[0], case, 2)["pc"]
            counts["reference_method"] = "pre-instruction Unicorn register/memory reads; guest capture audited"
            return direct_states[0], counts
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
    snapshot_version = getattr(args, "snapshot_version", 1)
    if args.group == "state" and snapshot_version != 2:
        raise ValueError("the focused architectural state probes require --snapshot-version=2")
    cases = d.full_state_cases() if args.group == "state" else d.corpus()
    if args.group not in ("all", "state"):
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
    report_path = output / "report.json"
    if report_path.is_file():
        previous = json.loads(report_path.read_text())
        if previous.get("snapshot_protocol", {"version": 1})["version"] != snapshot_version:
            raise ValueError("output contains evidence for another snapshot version; select a new directory")
    output.mkdir(parents=True, exist_ok=True)
    source_runner = runner
    source_digest = sha_file(source_runner)
    runner = output / "runner.loongarch"
    shutil.copy2(source_runner, runner)
    if sha_file(runner) != source_digest:
        raise ValueError("runner changed while freezing the qualification executable")
    report = {"version": 3, "scope": "arm64-guest-loongarch-qemu", "complete": False,
              "snapshot_protocol": d.snapshot_protocol(snapshot_version),
              "execution_engine": "core-step" if args.step else "core-run",
              "started": datetime.now(timezone.utc).isoformat(), "runner": str(runner),
              "source_runner": str(source_runner), "runner_sha256": source_digest, "qemu": args.qemu, "cpu": args.cpu,
              "simulator": simulator,
              "reference": oracle.versions, "group": args.group, "mode": args.mode, "environment": overrides,
              "sampling": {"method": "sha256-name" if args.sample_per_family is not None else "none",
                           "per_family": args.sample_per_family, "families": coverage,
                           "full_selected_group": len(cases) == sum(available.values())},
              "planned": len(cases), "executed": 0,
              "passed": 0, "failed": 0, "lbt_executed": False,
              "negative_controls_executed": 0, "negative_controls_rejected": 0, "cases": []}
    checkpoint(report_path, report)
    command_prefix = [str(qemu), "-L", str(args.sysroot), "-cpu", args.cpu]
    if args.runtime_libdir:
        command_prefix.extend(["-E", f"LD_LIBRARY_PATH={args.runtime_libdir}"])
    for case in cases:
        directory = output / case.name
        directory.mkdir(exist_ok=True)
        elf = d.make_elf(case, snapshot_version)
        elf_path = directory / "guest.elf"
        elf_path.write_bytes(elf)
        expected, counts = oracle.execute(case, elf, args.step, snapshot_version)
        (directory / "unicorn.bin").write_bytes(expected)
        errors = []
        proof = None
        negative_controls = None
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
                actual = d.decode_snapshot(result.stdout, case, snapshot_version)
                errors.extend(d.differences(d.decode_snapshot(expected, case, snapshot_version), actual))
                if snapshot_version == 2:
                    negative_controls = d.snapshot_negative_controls(result.stdout, case)
                    report["negative_controls_executed"] += negative_controls["executed"]
                    report["negative_controls_rejected"] += negative_controls["rejected"]
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
                                "negative_controls": negative_controls,
                                "step_proof": proof,
                                "unicorn_counts": counts if args.step or snapshot_version == 2 else None,
                                "snapshot_version": snapshot_version})
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
    parser.add_argument("--group", default="scalar", help="existing ARM64 corpus family, all, or focused v2 state probes")
    parser.add_argument("--step", action="store_true", help="execute Core Step and require retirement proof against Unicorn")
    parser.add_argument("--snapshot-version", type=int, choices=[1, 2], default=1,
                        help="explicit snapshot protocol: legacy GPR/memory v1 or complete architecture v2")
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
