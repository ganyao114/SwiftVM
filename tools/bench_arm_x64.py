#!/usr/bin/env python3
"""Compare ARM guest execution and diagnostic compile cost on Linux/x64.

Builds freestanding static ELFs with Clang/LLD; no guest libc or cross sysroot
is needed. Every run checks an independently computed full-width result.
Ordinary timing and intrusive compile diagnostics are measured separately.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import struct
import subprocess
import time


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_programs(out: Path, clang: str, iterations: int) -> dict:
    programs = {}
    for isa in ("arm64", "arm32"):
        bits = 64 if isa == "arm64" else 32
        mask = (1 << bits) - 1
        for workload in ("integer", "memory", "fp"):
            count = min(iterations, 100_000) if workload == "fp" else iterations
            if isa == "arm64":
                header = f""".text
.global _start
_start:
 movz x1, #{count & 65535}
 movk x1, #{count >> 16}, lsl #16
 adr x4, result
 mov x0, #0
"""
                body = {
                    "integer": "1: adds x0,x0,#3\n eor x0,x0,x0,lsr #7\n",
                    "memory": "1: str x0,[x4]\n ldr x3,[x4]\n add x0,x3,#3\n",
                    "fp": " fmov d0,#1.0\n fmov d1,#1.5\n1: fadd d0,d0,d1\n",
                }[workload]
                tail = " subs x1,x1,#1\n b.ne 1b\n"
                tail += " str d0,[x4]\n" if workload == "fp" else " str x0,[x4]\n"
                tail += """ mov x1,x4
 mov x0,#1
 mov x2,#8
 mov x8,#64
 svc #0
 mov x0,#0
 mov x8,#93
 svc #0
.data
.balign 16
result: .quad 0
"""
            else:
                header = f""".syntax unified
.arm
.fpu neon-vfpv4
.text
.global _start
_start:
 movw r1, #{count & 65535}
 movt r1, #{count >> 16}
 ldr r4, =result
 mov r0, #0
"""
                body = {
                    "integer": "1: adds r0,r0,#3\n eor r0,r0,r0,lsr #7\n",
                    "memory": "1: str r0,[r4]\n ldr r3,[r4]\n add r0,r3,#3\n",
                    "fp": " vmov.f64 d0,#1.0\n vmov.f64 d1,#1.5\n1: vadd.f64 d0,d0,d1\n",
                }[workload]
                tail = " subs r1,r1,#1\n bne 1b\n"
                tail += " vstr d0,[r4]\n" if workload == "fp" else " str r0,[r4]\n"
                tail += f""" mov r1,r4
 mov r0,#1
 mov r2,#{8 if workload == 'fp' else 4}
 mov r7,#4
 svc #0
 mov r0,#0
 mov r7,#1
 svc #0
.data
.balign 16
result: .quad 0
"""
            name = f"{isa}-{workload}"
            source, executable = out / f"{name}.S", out / name
            source.write_text(header + body + tail)
            target = "aarch64-linux-gnu" if isa == "arm64" else "armv7a-linux-gnueabi"
            subprocess.run([clang, f"--target={target}", "-nostdlib", "-static",
                            "-fuse-ld=lld", "-Wl,-e,_start", "-Wl,--build-id=none",
                            "-Wl,-Ttext=0x410000", str(source), "-o", str(executable)],
                           check=True, capture_output=True)
            if workload == "fp":
                expected = struct.pack("<d", 1.0 + count * 1.5)
            else:
                value = 0
                if workload == "integer":
                    for _ in range(count):
                        value = (value + 3) & mask
                        value ^= value >> 7
                else:
                    value = (count * 3) & mask
                expected = value.to_bytes(bits // 8, "little")
            programs[name] = {"path": executable, "expected": expected,
                              "iterations": count, "sha256": digest(executable)}
    return programs


def launch(runner: Path, program: dict, scope: str, diagnostic: bool) -> dict:
    command = [str(runner), "--scope", scope, "--pin-slots", "0"]
    if diagnostic:
        command.append("--decode-timing")
    command.append(str(program["path"]))
    begin = time.perf_counter_ns()
    process = subprocess.run(command, capture_output=True, timeout=60)
    elapsed = time.perf_counter_ns() - begin
    if process.returncode or process.stdout != program["expected"]:
        raise RuntimeError(f"{command}: exit={process.returncode} "
                           f"output={process.stdout.hex()} expected={program['expected'].hex()} "
                           f"stderr={process.stderr.decode(errors='replace')[-2000:]}")
    report = next(json.loads(line) for line in process.stderr.decode().splitlines()
                  if '"measurement_kind"' in line)
    if report["interpreted_steps"] or not report["native_bytes"] or report["fallbacks"]:
        raise RuntimeError(f"{command}: expected native execution in the requested scope")
    return {"wall_ns": elapsed, **{key: report.get(key) for key in
            ("compile_ns", "frontend_ns", "optimize_ns", "backend_ns", "native_bytes",
             "spill_slots", "compile_attempts", "fallbacks")}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clang", default="clang-18")
    parser.add_argument("--iterations", type=int, default=3_000_000)
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--cpu", type=int)
    args = parser.parse_args()
    if not 0 < args.iterations < 2**32 or args.samples < 3:
        parser.error("iterations must be in 1..2^32-1 and samples must be at least 3")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    runners = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    programs = build_programs(args.out, args.clang, args.iterations)
    result = {"schema_version": 1, "runners": {key: {"path": str(path), "sha256": digest(path)}
              for key, path in runners.items()}, "affinity": sorted(os.sched_getaffinity(0)),
              "samples": args.samples, "workloads": {}}
    for name, program in programs.items():
        for scope in ("block", "function"):
            key = f"{name}/{scope}"
            entry = {"sha256": program["sha256"], "iterations": program["iterations"],
                     "expected_hex": program["expected"].hex()}
            for diagnostic in (False, True):
                mode = "compile_diagnostic" if diagnostic else "ordinary"
                measurements = {variant: [] for variant in runners}
                for variant, runner in runners.items():
                    launch(runner, program, scope, diagnostic)
                for sample in range(args.samples):
                    order = list(runners) if sample % 2 == 0 else list(reversed(runners))
                    for variant in order:
                        measurements[variant].append(launch(runners[variant], program,
                                                             scope, diagnostic))
                primary = "compile_ns" if diagnostic else "wall_ns"
                medians = {variant: statistics.median(row[primary] for row in rows)
                           for variant, rows in measurements.items()}
                entry[mode] = {"primary": primary, "median_ns": medians,
                               "candidate_over_baseline": medians["candidate"] / medians["baseline"],
                               "measurements": measurements}
            result["workloads"][key] = entry
            (args.out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            print(f"{key}: runtime={entry['ordinary']['candidate_over_baseline']:.3f}x "
                  f"compile={entry['compile_diagnostic']['candidate_over_baseline']:.3f}x",
                  flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
