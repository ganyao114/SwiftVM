#!/usr/bin/env python3
"""Compare callable ARM64 kernels in SwiftVM and unmodified Dynarmic on Linux/x64.

First invocation includes lazy compilation; warm invocations reuse the same JIT.
Memory/register resets and result validation are outside the execution timers.
Cold-minus-warm is an estimate of first-invocation overhead, not a compiler clock.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mix(value: int) -> int:
    value &= MASK32
    value ^= value >> 16
    value = value * 0x7FEB352D & MASK32
    value ^= value >> 15
    value = value * 0x846CA68B & MASK32
    return value ^ (value >> 16)


def expected_compute() -> bytes:
    sha = hashlib.sha256(bytes((i * 37 + 11) & 255 for i in range(300)) * 1000).digest()
    values = sorted(mix(i + 17) for i in range(1024))
    ordered_sum = sum(value * (i + 1) for i, value in enumerate(values)) & MASK64
    hashed = 1
    for i in range(65536):
        hashed = mix(hashed + i)
    a = [(i + 1) * 0.03125 for i in range(64)]
    b = [(65 - i) * 0.125 for i in range(64)]
    fp = sum(sum(a[i * 8 + k] * b[k * 8 + j] for k in range(8))
             for i in range(8) for j in range(8))
    return struct.pack("<32sQIId", sha, ordered_sum, hashed, 0, fp)


def wrapper(size: int) -> str:
    return f""".text
.global _start
.type _start,%function
_start:
 bl bench_entry
 adrp x1,benchmark_output
 add x1,x1,:lo12:benchmark_output
 mov x0,#1
 mov x2,#{size}
 mov x8,#64
 svc #0
 mov x0,#0
 mov x8,#93
 svc #0
.size _start,.-_start
"""


def build_program(out: Path, name: str, count: int, clang: str, qemu: Path) -> dict:
    sources = []
    compute = name in ("compute", "compute-fma", "matrix-fma")
    if compute:
        expected = expected_compute()
        if name == "matrix-fma":
            expected = bytes(48) + expected[48:]
        source = ROOT / "benchmarks/arm64_compute.c"
        sources.append(source)
        assembly = wrapper(len(expected))
    else:
        body = {
            "integer": "1: adds x0,x0,#3\n eor x0,x0,x0,lsr #7\n",
            "memory": "1: str x0,[x4]\n ldr x3,[x4]\n add x0,x3,#3\n",
            "shift": "1: ror x0,x0,#13\n eor x0,x0,x0,lsr #7\n add x0,x0,#3\n",
            "fp": " fmov d0,#1.0\n fmov d1,#1.5\n1: fadd d0,d0,d1\n",
            "cond": (" mov x2,#0\n1: add x0,x0,#1\n cmp x0,#7\n b.hi 2f\n"
                     " add x2,x2,#3\n2: cmp x0,#5\n b.lt 3f\n add x2,x2,#5\n"
                     "3: and x3,x0,#1\n cbz x3,4f\n add x2,x2,#7\n"
                     "4: tbnz x0,#1,5f\n add x2,x2,#11\n5:\n"),
            "cmpsep": ("1: subs x1,x1,#1\n add x0,x0,#3\n add x2,x0,#5\n"
                       " sub x2,x2,#5\n eor x3,x2,x2,lsr #7\n"
                       " eor x3,x3,x2,lsr #7\n ror x3,x3,#13\n"
                       " ror x3,x3,#51\n add x0,x3,#0\n"),
            "fpmix": (" fmov d1,#1.5\n fmov d2,#2.0\n fmov d3,#0.5\n"
                      "1: fdiv d4,d2,d3\n scvtf d5,x0\n fadd d5,d5,d1\n"
                      " fcvtzs x0,d5\n fabs d4,d4\n fmadd d6,d1,d3,d4\n"
                      " fcmp d6,d2\n b.gt 2f\n add x0,x0,#7\n2:\n"),
        }[name]
        if name == "fp":
            expected = struct.pack("<d", 1.0 + count * 1.5)
        else:
            value = 0
            if name == "integer":
                for _ in range(count):
                    value = (value + 3) & MASK64
                    value ^= value >> 7
            elif name == "shift":
                for _ in range(count):
                    value = ((value >> 13) | (value << 51)) & MASK64
                    value ^= value >> 7
                    value = (value + 3) & MASK64
            elif name == "cond":
                bit1_clear = count // 4 * 2 + int(count % 4 >= 1)
                value = 3 * min(count, 7) + 5 * max(count - 4, 0) + 7 * ((count + 1) // 2) + 11 * bit1_clear
            elif name == "fpmix":
                value = count
            else:
                value = count * 3 & MASK64
            expected = value.to_bytes(8, "little")
        assembly = wrapper(8) + f"""
.global bench_entry
.type bench_entry,%function
bench_entry:
 movz x1,#{count & 65535}
 movk x1,#{count >> 16},lsl #16
 adrp x4,benchmark_output
 add x4,x4,:lo12:benchmark_output
 mov x0,#0
""" + body + (" b.ne 1b\n" if name == "cmpsep" else " subs x1,x1,#1\n b.ne 1b\n")
        if name == "cond":
            assembly += " mov x0,x2\n"
        assembly += " str d0,[x4]\n" if name == "fp" else " str x0,[x4]\n"
        assembly += """ ret
.size bench_entry,.-bench_entry
.data
.balign 16
.global benchmark_output
.type benchmark_output,%object
.size benchmark_output,8
benchmark_output: .quad 0
"""
    asm = out / f"{name}.S"
    asm.write_text(assembly)
    sources.append(asm)
    elf = out / name
    command = [clang, "--target=aarch64-linux-gnu", "-ffreestanding", "-fno-builtin",
               "-fomit-frame-pointer", "-fno-vectorize", "-fno-slp-vectorize",
               "-ffp-contract=off", "-O2", "-nostdlib", "-static", "-fuse-ld=lld",
               "-Wl,-e,_start", "-Wl,--build-id=none", "-Wl,-Ttext=0x410000",
               *map(str, sources), "-o", str(elf)]
    if name in ("compute-fma", "matrix-fma"):
        command[command.index("-ffp-contract=off")] = "-ffp-contract=on"
    if name == "matrix-fma":
        command.remove("-fno-vectorize")
        command.remove("-fno-slp-vectorize")
        command.insert(1, "-DSVM_BENCH_MATRIX_ONLY=1")
    subprocess.run(command, capture_output=True, check=True)
    symbols = {}
    for line in subprocess.check_output(["nm", "--defined-only", str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in ("bench_entry", "benchmark_output"):
            symbols[fields[2]] = int(fields[0], 16)
    if len(symbols) != 2:
        raise RuntimeError(f"{name}: missing benchmark symbols")
    reference = subprocess.run([str(qemu), str(elf)], capture_output=True, timeout=60, check=True)
    if reference.stdout != expected:
        raise RuntimeError(f"{name}: QEMU {reference.stdout.hex()} != independent {expected.hex()}")
    return {"path": str(elf), "sha256": digest(elf), "iterations": None if compute else count,
            "expected_hex": expected.hex(), "symbols": symbols, "build_command": command,
            "sources": {str(source): digest(source) for source in sources}, "qemu_verified": True}


def launch(runner: Path, config: dict, guest: dict, repeats: int) -> dict:
    symbols = guest["symbols"]
    command = [str(runner), "--elf", guest["path"], "--entry", hex(symbols["bench_entry"]),
               "--output", hex(symbols["benchmark_output"]), "--output-size",
               str(len(bytes.fromhex(guest["expected_hex"]))), "--expected", guest["expected_hex"],
               "--repeats", str(repeats), "--scope", config["scope"],
               "--pin-slots", str(config["pins"])]
    begin = time.perf_counter_ns()
    process = subprocess.run(command, capture_output=True, timeout=60)
    wall = time.perf_counter_ns() - begin
    if process.returncode:
        raise RuntimeError(f"{command}: exit {process.returncode}; {process.stderr.decode(errors='replace')}")
    row = json.loads(process.stdout)
    if row["engine"] != config["engine"] or not row["output_verified"] or not row["warm_cache_verified"]:
        raise RuntimeError(f"invalid runner result: {row}")
    if row["engine"] == "swiftvm" and row.get("effective_pin_slots", config["pins"]) != config["pins"]:
        raise RuntimeError(f"requested pin configuration did not take effect: {row}")
    if row["fallbacks"] or row["interpreted_steps"] or row["data_callbacks"]:
        raise RuntimeError(f"unexpected slow path: {row}")
    if len(row["warm_run_ns"]) != repeats or not row["compile_observations"]:
        raise RuntimeError(f"missing execution observations: {row}")
    row["process_wall_ns"] = wall
    row["warm_median_ns"] = statistics.median(row["warm_run_ns"])
    row["cold_extra_estimate_ns"] = row["cold_run_ns"] - row["warm_median_ns"]
    return row


def distribution(values: list[float]) -> dict:
    quartiles = statistics.quantiles(values, n=4, method="inclusive")
    return {"median": statistics.median(values), "q1": quartiles[0], "q3": quartiles[2],
            "min": min(values), "max": max(values)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--swiftvm", type=Path, required=True)
    parser.add_argument("--baseline-swiftvm", type=Path)
    parser.add_argument("--reuse-guests", type=Path, help="Original summary.json; reuse verified ELF bytes")
    parser.add_argument("--scope", action="append", choices=("block", "function"))
    parser.add_argument("--pin-slots", action="append", type=int, choices=(0, 2))
    parser.add_argument("--dynarmic", type=Path, required=True)
    parser.add_argument("--dynarmic-source", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clang", default="clang-18")
    parser.add_argument("--cpu", type=int, default=2)
    parser.add_argument("--samples", type=int, default=31)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=12_000_000)
    parser.add_argument("--fp-iterations", type=int, default=1_000_000)
    parser.add_argument("--workload", action="append", choices=("integer", "memory", "shift", "fp", "compute",
                                                              "cond", "cmpsep", "fpmix", "compute-fma", "matrix-fma"))
    args = parser.parse_args()
    if args.samples < 3 or not 1 <= args.repeats <= 100 or not 0 < args.iterations < 2**32 or not 0 < args.fp_iterations < 2**32:
        parser.error("invalid sample, repetition or iteration count")
    os.sched_setaffinity(0, {args.cpu})
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    configs = {f"swiftvm-{scope}-pin{pins}": {"engine": "swiftvm", "scope": scope, "pins": pins}
               for scope in args.scope or ("block", "function") for pins in args.pin_slots or (0, 2)}
    if args.baseline_swiftvm:
        configs.update({key.replace("swiftvm-", "baseline-", 1): {**config, "runner": "baseline"}
                        for key, config in list(configs.items())})
    configs["dynarmic"] = {"engine": "dynarmic", "scope": "block", "pins": 0}
    runners = {"swiftvm": args.swiftvm.resolve(), "dynarmic": args.dynarmic.resolve()}
    if args.baseline_swiftvm:
        runners["baseline"] = args.baseline_swiftvm.resolve()
    reused = json.loads(args.reuse_guests.read_text()) if args.reuse_guests else None
    source_paths = ["CMakeLists.txt", "benchmarks/arm64_compare.cpp", "benchmarks/arm64_compute.c",
                    "benchmarks/dynarmic/CMakeLists.txt", "tools/bench_dynarmic.py",
                    "source/runtime/backend/x64/emit.cpp", "source/runtime/backend/x64/emit.h",
                    "source/runtime/backend/x64/emit_memory.cpp", "source/runtime/backend/x64/emit_operand.cpp",
                    "source/runtime/backend/x64/emit_scalar.cpp"]
    result = {"schema_version": 1, "source_sha256": {name: digest(ROOT / name) for name in source_paths},
              "environment": {"platform": platform.platform(),
              "host_compiler": subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
              "guest_compiler": subprocess.check_output([args.clang, "--version"], text=True).splitlines()[0],
              "build": "Release with IPO for both runners and JIT libraries",
              "cpu": next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                          if line.startswith("model name")), "affinity": sorted(os.sched_getaffinity(0))},
              "swiftvm_commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
              "dynarmic_commit": subprocess.check_output(["git", "-C", str(args.dynarmic_source), "rev-parse", "HEAD"], text=True).strip(),
              "dynarmic_source_status": subprocess.check_output(["git", "-C", str(args.dynarmic_source), "status", "--porcelain"], text=True),
              "runners": {name: {"path": str(path), "sha256": digest(path)} for name, path in runners.items()},
              "qemu": {"path": str(args.qemu.resolve()), "sha256": digest(args.qemu)},
              "samples": args.samples, "warm_repeats_per_process": args.repeats,
              "configs": configs, "protocol": {
                  "timed_region": "CallGuest or Jit::Run only; initialization, reset and full output checks excluded",
                  "cold": "first invocation, includes lazy compilation and kernel execution",
                  "warm": "median of repeated invocations per fresh process; no new compilation permitted",
                  "cold_extra": "cold minus warm estimate; not a standalone Dynarmic compiler clock",
                  "order": "rotated and reversed across fresh processes; all children inherit CPU affinity",
                  "memory": "guest==host direct access; every writable segment and stack restored outside timing",
                  "dynarmic": "all safe optimizations; unsafe off; fastmem base 0, 64 bits; cycle counting off; host feature detection on",
                  "scope": "Dynarmic block JIT with its normal linking; SwiftVM Block/Function with pin 0/2",
                  "compile_observations": "SwiftVM compile attempts; Dynarmic source-code fetches; counters only verify warm cache reuse",
              }, "workloads": {}}
    if result["dynarmic_source_status"]:
        raise RuntimeError("Dynarmic must be unmodified")
    for name in args.workload or ("integer", "memory", "shift", "fp", "compute"):
        if reused:
            guest = reused["workloads"][name]["guest"]
            if digest(Path(guest["path"])) != guest["sha256"]:
                raise RuntimeError(f"original guest changed: {name}")
            reference = subprocess.run([str(args.qemu), guest["path"]], capture_output=True,
                                       timeout=60, check=True)
            if reference.stdout != bytes.fromhex(guest["expected_hex"]):
                raise RuntimeError(f"reused guest QEMU result differs: {name}")
        else:
            guest = build_program(args.out, name, args.fp_iterations if name in ("fp", "fpmix") else args.iterations,
                                  args.clang, args.qemu)
        print(f"{name}: QEMU and independent reference agree", flush=True)
        for config in configs.values():
            launch(runners[config.get("runner", config["engine"])], config, guest, args.repeats)
        measurements = {name: [] for name in configs}
        for sample in range(args.samples):
            order = list(configs)
            rotation = sample % len(order)
            order = order[rotation:] + order[:rotation]
            if sample % 2:
                order.reverse()
            for key in order:
                config = configs[key]
                measurements[key].append(launch(runners[config.get("runner", config["engine"])], config, guest, args.repeats))
        medians = {key: {metric: statistics.median(row[metric] for row in rows)
                        for metric in ("warm_median_ns", "cold_run_ns", "initialization_ns", "cold_extra_estimate_ns")}
                   for key, rows in measurements.items()}
        ratios = {}
        for key in configs:
            if key == "dynarmic":
                continue
            paired = [row["warm_median_ns"] / reference["warm_median_ns"]
                      for row, reference in zip(measurements[key], measurements["dynarmic"])]
            ratios[key] = {"warm_ratio_of_medians": medians[key]["warm_median_ns"] / medians["dynarmic"]["warm_median_ns"],
                           "paired_warm_ratio": distribution(paired),
                           "swiftvm_faster_pairs": sum(ratio < 1 for ratio in paired)}
        result["workloads"][name] = {"guest": guest, "medians": medians, "swiftvm_over_dynarmic": ratios,
                                     "measurements": measurements}
        if args.baseline_swiftvm:
            comparisons = {}
            for key in configs:
                if not key.startswith("swiftvm-"):
                    continue
                baseline = key.replace("swiftvm-", "baseline-", 1)
                paired = [row["warm_median_ns"] / reference["warm_median_ns"]
                          for row, reference in zip(measurements[key], measurements[baseline])]
                comparisons[key] = {
                    "warm_ratio_of_medians": medians[key]["warm_median_ns"] / medians[baseline]["warm_median_ns"],
                    "paired_warm_ratio": distribution(paired)}
            result["workloads"][name]["candidate_over_baseline"] = comparisons
        (args.out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        print(name, "warm ns", {key: round(value["warm_median_ns"]) for key, value in medians.items()}, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
