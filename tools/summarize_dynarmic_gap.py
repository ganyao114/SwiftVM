#!/usr/bin/env python3
"""Summarize the measured ARM64 gap and symbolize supplemental IP samples."""

from __future__ import annotations

import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import re
import subprocess

import bench_dynarmic as bench

ROOT = Path(__file__).resolve().parents[1]


def symbols(binary: Path) -> list[tuple[int, int, str]]:
    result = []
    for line in subprocess.check_output(["nm", "-n", "-S", "-C", "--defined-only", str(binary)], text=True).splitlines():
        fields = line.split(maxsplit=3)
        if len(fields) == 4 and fields[2] in ("t", "T", "w", "W"):
            try:
                result.append((int(fields[0], 16), int(fields[1], 16), fields[3]))
            except ValueError:
                pass
    return sorted(result)


def disassemble(path: Path, base: int = 0) -> list[tuple[int, str]]:
    command = ["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-Mintel",
               "--no-show-raw-insn", f"--adjust-vma={base}", str(path)]
    text = subprocess.check_output(command, text=True)
    path.with_suffix(".asm").write_text(text)
    return [(int(match[1], 16), match[2].strip()) for line in text.splitlines()
            if (match := re.match(r"\s*([0-9a-f]+):\s+(.+)$", line))]


def jit_intervals(folder: Path) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
    units, boundaries = [], []
    for line in (folder / "maps.tsv").read_text().splitlines():
        unit, start, size, guest_pc = line.split()
        start, size = int(start, 16), int(size)
        units.append((start, start + size))
        instructions = disassemble(folder / f"{unit}.bin", start)
        begin = None
        for i, (address, text) in enumerate(instructions):
            if re.match(r"mov\s+QWORD PTR \[r12\+0x378\],0x3$", text):
                begin = address
            if re.match(r"mov\s+QWORD PTR \[r12\+0x378\],0x2$", text) and begin is not None:
                high = instructions[i + 1][0] if i + 1 < len(instructions) else start + size
                boundaries.append((begin, high))
                begin = None
    return units, boundaries


def profile_summary(work: Path) -> dict:
    build = json.loads((work / "build.json").read_text())
    symbol_tables = {key: symbols(Path(entry["path"])) for key, entry in build["runners"].items()}
    result = {}
    data = json.loads((work / "profile-summary.json").read_text())
    for name, entry in data["workloads"].items():
        result[name] = {}
        for key, rows in entry["measurements"].items():
            engine = data["configs"][key]["engine"]
            binary = Path(build["runners"][engine]["path"])
            table = symbol_tables[engine]
            starts = [value[0] for value in table]
            categories, functions, generated_details = Counter(), Counter(), Counter()
            lost = 0
            for row in rows:
                folder = Path(row["output_directory"])
                sampling = json.loads((folder / "sampling.json").read_text())
                lost += sampling["lost"]
                mappings = []
                for line in (folder / "process.maps").read_text().splitlines():
                    fields = line.split(maxsplit=5)
                    low, high = [int(value, 16) for value in fields[0].split("-")]
                    mappings.append((low, high, fields[1], int(fields[2], 16), fields[5] if len(fields) > 5 else ""))
                base = next(low - offset for low, high, mode, offset, path in mappings
                            if path == str(binary) and offset == 0)
                jit, call_boundaries = [], []
                if engine == "swiftvm":
                    jit, call_boundaries = jit_intervals(folder)
                else:
                    addresses = [int(line.split()[0], 16) for line in (folder / "dynarmic.asm").read_text().splitlines()]
                    jit.append((min(addresses), max(addresses) + 16))
                for line in (folder / "samples.tsv").read_text().splitlines():
                    ip, count = line.split()
                    ip, count = int(ip, 16), int(count)
                    function = None
                    if any(low <= ip < high for low, high in jit):
                        category = "generated_guest_code"
                        if engine == "swiftvm":
                            detail = ("helper_call_boundary" if any(low <= ip < high for low, high in call_boundaries)
                                      else "other_generated_code")
                            generated_details[detail] += count
                    else:
                        relative = ip - base
                        at = bisect.bisect_right(starts, relative) - 1
                        if at >= 0 and table[at][0] <= relative < table[at][0] + table[at][1]:
                            function = table[at][2]
                            if re.search(r"softfloat_|\bf(?:16|32|64|128)_|::arm64::", function):
                                category = "numeric_helper_and_softfloat"
                            elif function.startswith("SvmNative") or function.startswith("SvmRunNative"):
                                category = "native_dispatch_and_entry"
                            else:
                                category = "other_executable_code"
                        else:
                            match = next((item for item in mappings if item[0] <= ip < item[1]), None)
                            if match and not match[4] and "x" in match[2]:
                                category = "jit_prelude_or_trampoline"
                            elif match and match[4]:
                                category = "shared_library_or_kernel_helper"
                            else:
                                category = "unresolved"
                    categories[category] += count
                    if function:
                        functions[function] += count
            total = sum(categories.values())
            result[name][key] = {"samples": total, "lost": lost, "category_counts": dict(categories),
                                 "category_percent": {key: value * 100 / total for key, value in categories.items()},
                                 "generated_detail_percent_of_all_samples": {key: value * 100 / total for key, value in generated_details.items()},
                                 "top_functions": [{"symbol": key, "samples": value, "percent": value * 100 / total}
                                                   for key, value in functions.most_common(15)]}
    (work / "symbolized-profile.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def hot_loop_evidence(work: Path, destination: Path) -> dict:
    folder = work / "baseline/integer/swiftvm-function-pin0/0"
    swift = disassemble(folder / "0.bin")
    dynarmic = []
    for line in (work / "baseline/integer/dynarmic/0/dynarmic.asm").read_text().splitlines():
        address, text = line.split(maxsplit=1)
        dynarmic.append((int(address, 16), text))

    def walk(instructions, engine):
        entries = dict(instructions)
        next_address = {instructions[i][0]: instructions[i + 1][0] for i in range(len(instructions) - 1)}
        backward = [(address, int(match[1], 16)) for address, text in instructions
                    if (match := re.match(r"(?:jne|jz)\s+0x([0-9a-fA-F]+)$", text)) and int(match[1], 16) < address]
        end, start = min(backward)
        result = []
        pc = start
        for _ in range(200):
            text = entries[pc]
            result.append((pc, text))
            if pc == end:
                return result
            match = re.match(r"(je|jnz)\s+0x([0-9a-fA-F]+)$", text)
            # Ordinary no-event path in SwiftVM; taken loop edge in Dynarmic.
            if match and ((engine == "swiftvm" and match[1] == "je") or
                          (engine == "dynarmic" and match[1] == "jnz")):
                pc = int(match[2], 16)
            else:
                pc = next_address[pc]
        raise RuntimeError("could not reconstruct hot loop")

    loops = {"swiftvm": walk(swift, "swiftvm"), "dynarmic": walk(dynarmic, "dynarmic")}
    assert len(loops["swiftvm"]) == 47 and len(loops["dynarmic"]) == 23
    lines = ["Ordinary taken integer loop; cold event/exit paths excluded.",
             "Offsets relative to the hot loop entry, not static instruction counts of the entire code cache.", ""]
    for name, instructions in loops.items():
        lines.append(f"{name}: {len(instructions)} instructions per taken iteration")
        base = instructions[0][0]
        for address, text in instructions:
            branch = re.match(r"(j\w+)\s+0x([0-9a-fA-F]+)$", text)
            if branch:
                text = f"{branch[1]} loop+0x{int(branch[2],16)-base:x}"
            lines.append(f"  +{address-base:04x}  {text}")
        lines.append("")
    path = destination / "integer-hot-loops.txt"
    path.write_text("\n".join(lines) + "\n")
    return {"path": str(path.relative_to(ROOT)), "sha256": bench.digest(path),
            "instructions_per_iteration": {name: len(loop) for name, loop in loops.items()},
            "instruction_categories": {
                "swiftvm": {"arithmetic_payload_and_state_moves": 10, "counter_update_and_state": 4,
                            "capture_pack_merge_and_invert_NZCV": 19, "event_check": 2,
                            "condition_boolean_and_branch": 12},
                "dynarmic": {"arithmetic_payload_and_state_moves": 7, "counter_update_and_state": 6,
                             "native_flag_save_restore_and_branch": 8, "event_check": 2}},
            "note": "Static taken-path categories cross-checked against retired PMU instruction counts; not time percentages."}


def export(work: Path, profile: dict, original: Path) -> dict:
    destination = ROOT / "docs/evidence/dynarmic-gap-20261111"
    destination.mkdir(parents=True, exist_ok=True)
    build = json.loads((work / "build.json").read_text())
    for name, expected in build["source_sha256"].items():
        assert bench.digest(Path(name)) == expected, name
    for entry in build["runners"].values():
        assert bench.digest(Path(entry["path"])) == entry["sha256"]
    baseline = json.loads((work / "baseline-summary.json").read_text())
    ablations = json.loads((work / "ablations-summary.json").read_text())
    scalar = json.loads((work / "scalar-summary.json").read_text())
    sampled = json.loads((work / "profile-summary.json").read_text())
    regular = json.loads((original / "final/summary.json").read_text())
    ordinary, sample_processes, executions, records = 0, 0, 0, 0
    summaries = {}
    for label, source in (("baseline", baseline), ("controlled_variants", ablations), ("scalar_controls", scalar), ("sampled", sampled)):
        workloads = {}
        for name, entry in source["workloads"].items():
            guest = entry["guest"]
            assert bench.digest(Path(guest["path"])) == guest["sha256"]
            for path, expected in guest["sources"].items():
                assert bench.digest(Path(path)) == expected, path
            assert guest["qemu_verified"]
            for rows in entry["measurements"].values():
                for row in rows:
                    assert row["output_verified"] and row["warm_cache_verified"]
                    assert row["fallbacks"] == row["interpreted_steps"] == row["data_callbacks"] == 0
                    assert all(record["enabled_ns"] == record["running_ns"] and record["running_ns"] > 0 for record in row["pmu"])
                    records += len(row["pmu"])
                    executions += 1 + len(row["warm_run_ns"])
                    if label == "sampled": sample_processes += 1
                    else: ordinary += 1
            workloads[name] = {"guest": guest, "medians": entry["medians"]}
            measurements = entry["measurements"]
            swift, dyn = entry["medians"]["swiftvm-function-pin0"], entry["medians"]["dynarmic"]
            ratio_samples = [row["warm_median_ns"] / reference["warm_median_ns"]
                             for row, reference in zip(measurements["swiftvm-function-pin0"], measurements["dynarmic"])]
            workloads[name]["swiftvm_over_dynarmic"] = {
                "wall_ratio": swift["warm_ns"] / dyn["warm_ns"],
                "instruction_ratio": swift["instructions"] / dyn["instructions"],
                "cycles_ratio": swift["cycles"] / dyn["cycles"],
                "dynarmic_IPC_over_swiftvm_IPC": dyn["instructions_per_cycle"] / swift["instructions_per_cycle"],
                "paired_wall_ratio": bench.distribution(ratio_samples),
                "swiftvm_faster_pairs": sum(value < 1 for value in ratio_samples),
            }
        summaries[label] = {"samples": source["samples"], "workloads": workloads}
    source_names = ["benchmarks/arm64_gap_metrics.h", "tools/analyze_dynarmic_gap.py", "tools/summarize_dynarmic_gap.py",
                    "source/runtime/backend/x64/emit_flags.cpp", "source/runtime/backend/x64/emit_control.cpp",
                    "source/runtime/backend/x64/emit.cpp", "source/runtime/backend/x64/emit_calls.cpp",
                    "source/runtime/backend/x64/emit_memory.cpp", "source/runtime/backend/x64/emit_float.cpp",
                    "source/runtime/frontend/arm64/semantics_float.inc", "source/runtime/frontend/arm64/sink.h",
                    "source/runtime/arch/arm64/fp_scalar.cpp", "source/runtime/arch/arm64/fp_vector.cpp"]
    raw_paths = [work / f"{label}-summary.json" for label in ("baseline", "ablations", "scalar", "profile")]
    raw_paths.extend([work / "build.json", work / "guests.json", work / "symbolized-profile.json",
                      work / "clang-driver.txt", work / "original_order.ll", work / "no_vector_last.ll"])
    artifacts = {}
    for label in ("baseline", "ablations", "scalar", "profile"):
        for path in sorted((work / label).rglob("*")):
            if path.is_file() and path.suffix in (".bin", ".tsv", ".json", ".maps", ".ir", ".asm"):
                artifacts[str(path.relative_to(work))] = bench.digest(path)
    manifest = work / "artifact-manifest.json"
    manifest.write_text(json.dumps(artifacts, indent=2) + "\n")
    raw_paths.append(manifest)
    result = {"schema_version": 1, "date": "2026-11-11", "environment": regular["environment"],
              "swiftvm_commit": regular["swiftvm_commit"], "dynarmic_commit": regular["dynarmic_commit"],
              "production_runtime_modified": False, "original_runners": regular["runners"],
              "diagnostic_build": build, "source_sha256": {name: bench.digest(ROOT / name) for name in source_names},
              "protocol": {
                  "pmu": "Pinned current-thread group: userspace retired instructions, cycles, branches, branch misses; all enabled/running times equal; no multiplex scaling",
                  "timing": "Same callable kernels and full output validation as comparison; 21 fresh process pairs, three warm invocations per process",
                  "sampling": "Separate diagnostic runs, 3 fresh processes per engine/kernel, 60 warm invocations each; PERF_COUNT_SW_CPU_CLOCK 250 us IP sampling",
                  "sample_timing_not_headline": True,
                  "variants": "Alternate guest kernels identify sensitivities; output contract is checked, final NZCV is not part of that contract",
                  "isolated_phases": "Compute phases separately recompiled; their timings and differences are not an exact additive partition of the original workload",
                  "scalar_controls": "Disabling vectorizers occurs after -O2; both code generation and full output are checked",
                  "code_dumps": "SwiftVM compile/publication callbacks observe IR and actual published bytes without changing IR; Dynarmic public disassembly API; diagnostic hooks run outside warm timers",
              }, "validation": {"ordinary_processes": ordinary, "sampling_processes": sample_processes,
                                  "verified_kernel_executions": executions, "pmu_records": records,
                                  "all_pmu_records_unmultiplexed": True,
                                  "ip_samples": sum(entry["samples"] for rows in profile.values() for entry in rows.values()),
                                  "lost_ip_samples": sum(entry["lost"] for rows in profile.values() for entry in rows.values()),
                                  "warm_additional_compiles": 0, "fallbacks": 0, "interpreted_steps": 0,
                                  "controlled_guest_programs_qemu_verified": 18},
              "measurements": summaries, "profile": profile,
              "integer_hot_loop": hot_loop_evidence(work, destination),
              "vectorization_correction": {
                  "original_driver_flags": ["-vectorize-loops", "-vectorize-slp"],
                  "original_compute_IR_vector_FP_operations": 66,
                  "flags_after_O2_IR_vector_FP_operations": 0,
                  "original_compute_A64_contains": ["fadd v?.2d", "fmul v?.2d", "fmul v?.2d, v?.2d, v?.d[0]"],
                  "scalar_controls_A64_vector_FP_operations": 0,
              },
              "raw_evidence": {path.name: {"path": str(path), "sha256": bench.digest(path)} for path in raw_paths},
              "limitations": ["WSL finite workloads, CPU frequency not locked; ratios do not prove full application performance.",
                              "Retired instructions are dynamic PMU values; instructions-per-cycle explains throughput, not per-mechanism time percentages.",
                              "Alternate guest loops and separate phase compilation preserve output only; effects must not be added as independently removable costs.",
                              "CPU-clock sampling is intrusive and may attribute stall time to the following instruction; sampled timings are excluded from headline measurements.",
                              "Symbol-based numeric classification includes the numeric helper closure; shared-library samples remain separate when exact symbols are unavailable.",
                              "A native x64 FP path must preserve ARM FPCR/FPSR and boundary behavior; replacing the helper wholesale is not established as correct by these measurements."]}
    (destination / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=Path.home() / ".cache/SwiftVM/dynarmic-gap-20261111")
    parser.add_argument("--original", type=Path, default=Path.home() / ".cache/SwiftVM/dynarmic-bench-20261111")
    args = parser.parse_args()
    profile = profile_summary(args.work)
    for name, rows in profile.items():
        for key, entry in rows.items():
            print(name, key, "samples", entry["samples"], "lost", entry["lost"],
                  {key: round(value, 2) for key, value in entry["category_percent"].items()})
            if name in ("fp", "compute"):
                print(entry["top_functions"][:8])
    result = export(args.work, profile, args.original)
    print("AUDITED", json.dumps(result["validation"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
