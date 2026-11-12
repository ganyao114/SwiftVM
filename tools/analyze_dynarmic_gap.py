#!/usr/bin/env python3
"""Measure ARM64 JIT gaps with PMU counters and controlled guest variants.

Builds supplemental runners from the existing comparison harness by inserting
diagnostic hooks. Runtime/JIT library sources and ordinary runners are unchanged.
Generated source, build commands, outputs and hardware-counter records are kept.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess
import sys

import bench_dynarmic as bench

ROOT = Path(__file__).resolve().parents[1]


def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise RuntimeError(f"diagnostic insertion no longer matches: {old[:120]}")
    return source.replace(old, new, 1)


def prepare(work: Path, original: Path) -> dict:
    source = (ROOT / "benchmarks/arm64_compare.cpp").read_text()
    source = replace_once(source, "namespace {", f'#include "{ROOT}/benchmarks/arm64_gap_metrics.h"\n\nnamespace {{')
    source = replace_once(source, 'const char* Name() const { return "dynarmic"; }',
                          'const char* Name() const { return "dynarmic"; }\n'
                          '    void Diagnose() { gap::WriteDisassembly(jit->Disassemble()); }')
    source = replace_once(source, 'const char* Name() const { return "swiftvm"; }',
                          'const char* Name() const { return "swiftvm"; }\n'
                          '    void Diagnose() { gap::WriteStats(stats); }')
    source = replace_once(source,
                          'vm = std::make_unique<swift::runtime::execution::VM>(policy, stats, host.Bind());',
                          'auto backend = host.Bind();\n        gap::Wrap(backend);\n'
                          '        vm = std::make_unique<swift::runtime::execution::VM>(policy, stats, backend);')
    source = replace_once(source, 'const auto start = Clock::now();',
                          'gap::Start(run);\n            const auto start = Clock::now();')
    source = replace_once(source, 'times.push_back(Nanoseconds(start));',
                          'times.push_back(Nanoseconds(start));\n            gap::Stop(run);')
    source = replace_once(source, '        std::printf(\n                "{\\\"engine',
                          '        engine.Diagnose();\n        gap::Finish();\n'
                          '        std::printf(\n                "{\\\"engine')
    generated = work / "diagnostic_runner.cpp"
    generated.write_text(source)
    metadata = {"source_sha256": {str(path): bench.digest(path) for path in
                (ROOT / "benchmarks/arm64_compare.cpp", ROOT / "benchmarks/arm64_gap_metrics.h", generated)},
                "runners": {}, "commands": {}}
    for engine, build_dir, target in (
        ("swiftvm", ROOT / "build/host", "svm_arm64_compare_bench"),
        ("dynarmic", original / "runner-build", "dynarmic_arm64_compare_bench"),
    ):
        lines = subprocess.check_output(["ninja", "-t", "commands", target], cwd=build_dir, text=True).splitlines()
        tokens = shlex.split(lines[-1])
        start = next(i for i, value in enumerate(tokens) if value.endswith("/c++"))
        stop = tokens.index("&&", start) if "&&" in tokens[start:] else len(tokens)
        command = tokens[start:stop]
        positions = [i for i, value in enumerate(command) if value.endswith("arm64_compare.cpp.o")]
        if len(positions) != 1:
            raise RuntimeError("expected exactly one harness object")
        command[positions[0]] = str(generated)
        output = work / f"{engine}_gap_probe"
        command[command.index("-o") + 1] = str(output)
        compile_line = next(line for line in lines if " -c " in line and line.rstrip().endswith("arm64_compare.cpp"))
        compile_args = shlex.split(compile_line)
        includes = []
        at = 0
        while at < len(compile_args):
            item = compile_args[at]
            if item.startswith(("-I", "-D", "-std=")):
                includes.append(item)
            elif item == "-isystem":
                includes.extend(compile_args[at:at + 2])
                at += 1
            at += 1
        command[1:1] = includes
        if engine == "swiftvm":
            command.insert(command.index("libsvm_execution.a"), str(ROOT / "build/host/libsvm_ir_format.a"))
        result = subprocess.run(command, cwd=build_dir, capture_output=True, text=True)
        (work / f"{engine}-build.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr)[-4000:])
        metadata["runners"][engine] = {"path": str(output), "sha256": bench.digest(output)}
        metadata["commands"][engine] = {"cwd": str(build_dir), "argv": command}
        print(f"Built supplemental {engine} diagnostic runner", flush=True)
    (work / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def measure(work: Path, original: Path, samples: int, configs: dict, guests: dict,
            label: str, profile: bool = False) -> dict:
    build = json.loads((work / "build.json").read_text())
    runners = {name: Path(entry["path"]) for name, entry in build["runners"].items()}
    result = {"samples": samples, "profile": profile, "configs": configs,
              "runners": build["runners"], "workloads": {}}
    for name, guest in guests.items():
        rows = {key: [] for key in configs}
        for sample in range(samples):
            order = list(configs)
            rotation = sample % len(order)
            order = order[rotation:] + order[:rotation]
            if sample % 2:
                order.reverse()
            for key in order:
                output = work / label / name / key / str(sample)
                if output.exists() and any(output.iterdir()):
                    raise RuntimeError(f"diagnostic output already exists; choose a fresh --out: {output}")
                output.mkdir(parents=True, exist_ok=True)
                os.environ["SVM_GAP_OUT"] = str(output)
                if profile:
                    os.environ["SVM_GAP_SAMPLE"] = "1"
                else:
                    os.environ.pop("SVM_GAP_SAMPLE", None)
                row = bench.launch(runners[configs[key]["engine"]], configs[key], guest,
                                   60 if profile else 3)
                pmu = json.loads((output / "pmu.json").read_text())
                if len(pmu) != (61 if profile else 4):
                    raise RuntimeError("missing PMU records")
                if not all(item["running_ns"] == item["enabled_ns"] and item["running_ns"] > 0 for item in pmu):
                    raise RuntimeError("PMU unavailable or multiplexed")
                row["pmu"] = pmu
                row["output_directory"] = str(output)
                rows[key].append(row)
        medians = {}
        for key, measurements in rows.items():
            values = {metric: [statistics.median(row[metric] for row in sample["pmu"][1:])
                               for sample in measurements]
                      for metric in ("cycles", "instructions", "branches", "branch_misses")}
            medians[key] = {metric: statistics.median(items) for metric, items in values.items()}
            medians[key]["warm_ns"] = statistics.median(sample["warm_median_ns"] for sample in measurements)
            medians[key]["instructions_per_cycle"] = medians[key]["instructions"] / medians[key]["cycles"]
            medians[key]["branch_miss_fraction"] = medians[key]["branch_misses"] / medians[key]["branches"]
        result["workloads"][name] = {"guest": guest, "medians": medians, "measurements": rows}
        (work / f"{label}-summary.json").write_text(json.dumps(result, indent=2) + "\n")
        print(name, {key: {"ms": round(value["warm_ns"] / 1e6, 4), "Minsn": round(value["instructions"] / 1e6, 4),
                           "IPC": round(value["instructions_per_cycle"], 3)} for key, value in medians.items()}, flush=True)
    return result


def build_variants(work: Path, original: Path) -> dict:
    prior = json.loads((original / "final/summary.json").read_text())
    qemu = Path(prior["qemu"]["path"])
    output = work / "guests"
    output.mkdir(exist_ok=True)
    guests = {}

    def compile_guest(name: str, assembly: str, expected: bytes, count: int | None,
                      source: Path | None = None, scalar_only: bool = False) -> None:
        asm, elf = output / f"{name}.S", output / name
        asm.write_text(assembly)
        sources = [source, asm] if source else [asm]
        command = ["clang-18", "--target=aarch64-linux-gnu", "-ffreestanding", "-fno-builtin",
                   "-fomit-frame-pointer", "-fno-vectorize", "-fno-slp-vectorize", "-ffp-contract=off",
                   "-O2", "-nostdlib", "-static", "-fuse-ld=lld", "-Wl,-e,_start",
                   "-Wl,--build-id=none", "-Wl,-Ttext=0x410000", *map(str, sources), "-o", str(elf)]
        if scalar_only:
            # Clang 18 processes -O2 after earlier -fno-vectorize flags by
            # re-enabling the vectorizers. Put the disabling flags last.
            command.extend(["-fno-vectorize", "-fno-slp-vectorize"])
        process = subprocess.run(command, capture_output=True, text=True)
        if process.returncode:
            raise RuntimeError(process.stderr)
        symbols = {}
        for line in subprocess.check_output(["nm", "--defined-only", str(elf)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2] in ("bench_entry", "benchmark_output"):
                symbols[fields[2]] = int(fields[0], 16)
        process = subprocess.run([str(qemu), str(elf)], capture_output=True, check=True, timeout=60)
        if process.stdout != expected:
            raise RuntimeError(f"{name}: independent expectation != QEMU output")
        guests[name] = {"path": str(elf), "sha256": bench.digest(elf), "symbols": symbols,
                        "iterations": count, "expected_hex": expected.hex(), "build_command": command,
                        "sources": {str(path): bench.digest(path) for path in sources}, "qemu_verified": True}

    variants = {
        "empty-subs": ("empty", 1, False, False),
        "empty-cbnz": ("empty", 1, False, True),
        "integer-add": ("integer", 1, True, False),
        "integer-cbnz": ("integer", 1, True, True),
        "integer-u8": ("integer", 8, False, False),
        "integer-cbnz-u8": ("integer", 8, True, True),
        "memory-cbnz": ("memory", 1, False, True),
        "memory-u8": ("memory", 8, False, False),
        "shift-cbnz": ("shift", 1, False, True),
        "shift-u8": ("shift", 8, False, False),
        "fp-cbnz": ("fp", 1, False, True),
        "fp-u8": ("fp", 8, False, False),
    }
    for name, (family, unroll, no_add_flags, cbnz) in variants.items():
        count = 1_000_000 if family == "fp" else 12_000_000
        loops = count // unroll
        body = {"empty": "", "integer": f" {'add' if no_add_flags else 'adds'} x0,x0,#3\n eor x0,x0,x0,lsr #7\n",
                "memory": " str x0,[x4]\n ldr x3,[x4]\n add x0,x3,#3\n",
                "shift": " ror x0,x0,#13\n eor x0,x0,x0,lsr #7\n add x0,x0,#3\n",
                "fp": " fadd d0,d0,d1\n"}[family]
        assembly = bench.wrapper(8) + f"""
.global bench_entry
.type bench_entry,%function
bench_entry:
 movz x1,#{loops & 65535}
 movk x1,#{loops >> 16},lsl #16
 adrp x4,benchmark_output
 add x4,x4,:lo12:benchmark_output
 mov x0,#0
"""
        if family == "fp":
            assembly += " fmov d0,#1.0\n fmov d1,#1.5\n"
        assembly += "1:\n" + body * unroll
        assembly += " sub x1,x1,#1\n cbnz x1,1b\n" if cbnz else " subs x1,x1,#1\n b.ne 1b\n"
        assembly += " str d0,[x4]\n" if family == "fp" else " str x0,[x4]\n"
        assembly += """ ret
.size bench_entry,.-bench_entry
.data
.balign 16
.global benchmark_output
.type benchmark_output,%object
.size benchmark_output,8
benchmark_output: .quad 0
"""
        expected = bytes(8) if family == "empty" else bytes.fromhex(prior["workloads"][family]["guest"]["expected_hex"])
        compile_guest(name, assembly, expected, count)
        guests[name]["experiment"] = {"family": family, "unroll": unroll,
                                       "add_without_flags": no_add_flags, "counter_uses_cbnz": cbnz}

    source = (ROOT / "benchmarks/arm64_compute.c").read_text()
    begin = source.index("static void compute(void) {") + len("static void compute(void) {")
    end = source.index("\n}\n\nvoid bench_entry", begin)
    marks = [begin, source.index("    for (unsigned i = 0; i < 1024; ++i)", begin),
             source.index("    uint32_t hash = 1;", begin),
             source.index("    for (unsigned i = 0; i < 64; ++i)", begin), end]
    full_expected = bytes.fromhex(prior["workloads"]["compute"]["guest"]["expected_hex"])
    for i, (phase, low, high) in enumerate((("sha", 0, 32), ("sort", 32, 40), ("mix", 40, 44), ("matrix", 48, 56))):
        guest_source = output / f"compute-{phase}.c"
        guest_source.write_text(source[:begin] + "\n" + source[marks[i]:marks[i + 1]] + source[end:])
        expected = bytearray(56)
        expected[low:high] = full_expected[low:high]
        compile_guest(f"compute-{phase}", bench.wrapper(56), bytes(expected), None, guest_source)
        guests[f"compute-{phase}"]["experiment"] = {"phase": phase, "separately_compiled": True,
                                                    "timings_are_not_additive": True}
    for phase, source_path in (("matrix", output / "compute-matrix.c"),
                               ("full", ROOT / "benchmarks/arm64_compute.c")):
        expected = full_expected if phase == "full" else bytes(48) + full_expected[48:56]
        compile_guest(f"compute-{phase}-scalar", bench.wrapper(56), expected, None, source_path, True)
        guests[f"compute-{phase}-scalar"]["experiment"] = {"phase": phase, "vectorization_disabled_after_O2": True}
    (work / "guests.json").write_text(json.dumps(guests, indent=2) + "\n")
    print(f"Built and independently/QEMU checked {len(guests)} controlled kernels", flush=True)
    return guests


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, default=Path.home() / ".cache/SwiftVM/dynarmic-bench-20261111")
    parser.add_argument("--out", type=Path, default=Path.home() / ".cache/SwiftVM/dynarmic-gap-20261111")
    parser.add_argument("--stage", choices=("build", "guests", "baseline", "ablations", "scalar", "profile"), required=True)
    parser.add_argument("--samples", type=int, default=21)
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("samples must be positive")
    os.sched_setaffinity(0, {2})
    args.out.mkdir(parents=True, exist_ok=True)
    if args.stage == "build":
        prepare(args.out, args.original)
    elif args.stage == "guests":
        build_variants(args.out, args.original)
    else:
        original = json.loads((args.original / "final/summary.json").read_text())
        configs = {"swiftvm-function-pin0": {"engine": "swiftvm", "scope": "function", "pins": 0},
                   "dynarmic": {"engine": "dynarmic", "scope": "block", "pins": 0}}
        guests = (json.loads((args.out / "guests.json").read_text()) if args.stage in ("ablations", "scalar")
                  else {key: entry["guest"] for key, entry in original["workloads"].items()})
        if args.stage == "ablations":
            guests = {key: value for key, value in guests.items() if not key.endswith("-scalar")}
        elif args.stage == "scalar":
            guests = {key: value for key, value in guests.items() if key.endswith("-scalar")}
        measure(args.out, args.original, args.samples, configs, guests, args.stage, args.stage == "profile")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
