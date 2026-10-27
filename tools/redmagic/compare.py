#!/usr/bin/env python3
"""Validate a complete CPU-4 benchmark batch and compare paired two-point costs.

All observations are retained. An invalid point makes the batch invalid; this
tool never chooses a fastest sample or turns a measurement into an R0/R7 pass.
"""

import argparse
import csv
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import statistics
import sys


EVENTS = ("cpu-cycles", "instructions")
CRC = {2000: "0x4983", 12000: "0xd340"}
CHILD_WRAPPER = 'printf "SVM_BENCH_PID=%s\\n" "$$" >&2; /system/bin/taskset -p $$ >&2; exec "$@"'
WRAPPER = 'sample=$1; child_wrapper=$2; shift 2; printf "SVM_BENCH_COUNTER_PID=%s\\n" "$$" >&2; /system/bin/sh -c "$child_wrapper" pmu-target "$@" & target=$!; printf "SVM_BENCH_CHILD_PID=%s\\n" "$target" >&2; if wait "$target"; then status=0; else status=$?; fi; printf "%s\\n" "$status" > "$sample/target-exit-code.txt"; exit "$status"'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pairs(text):
    result = {}
    for line in text.splitlines():
        key, value = line.split("=", 1)
        require(key not in result, f"duplicate key {key}")
        result[key] = value
    return result


def identities(text):
    result = {}
    for line in text.splitlines():
        digest, path = line.split(None, 1)
        require(path not in result, f"duplicate identity {path}")
        result[path] = digest
    return result


def read_argv(path):
    data = path.read_bytes()
    require(data.endswith(b"\0"), f"unterminated argv in {path}")
    return [word.decode() for word in data[:-1].split(b"\0")]


def parse_pmu(path):
    raw, totals, seconds = [], {}, None
    for row in csv.reader(path.read_text().splitlines()):
        if row and row[-1] == "":
            row.pop()
        if not row:
            continue
        if row[0] in EVENTS:
            require(len(row) == 13, f"unknown verbose PMU row: {row}")
            fields = dict(zip(row[1::2], row[2::2]))
            require(set(fields) == {"tid", "cpu", "count", "time_enabled", "time running", "id"}, "unknown raw PMU fields")
            entry = {k: int(v) for k, v in fields.items()}
            entry["event"] = row[0]
            require(entry["cpu"] == 4 and entry["tid"] > 0 and entry["count"] > 0, "invalid PMU CPU, tid or count")
            require(entry["time_enabled"] == entry["time running"] > 0, "PMU was not running for all enabled time")
            entry["running_enabled_ratio"] = entry["time running"] / entry["time_enabled"]
            raw.append(entry)
        elif len(row) > 1 and row[1] in EVENTS:
            require(row[1] not in totals, "duplicate PMU total")
            totals[row[1]] = int(row[0])
        elif row[0] == "Total test time":
            require(seconds is None and row[2] == "seconds", "duplicate/unknown elapsed time")
            seconds = float(row[1])
        else:
            require(row == ["Performance counter statistics"], f"unknown PMU output: {row}")
    require(set(totals) == set(EVENTS) and raw, "missing PMU events or raw running times")
    require(seconds is not None and math.isfinite(seconds) and seconds > 0, "invalid wall time")
    for key, group in itertools.groupby(sorted(raw, key=lambda r: (r["tid"], r["cpu"])), key=lambda r: (r["tid"], r["cpu"])):
        group = list(group)
        require(len(group) == 2 and {r["event"] for r in group} == set(EVENTS), f"incomplete event group {key}")
        require(group[0]["time_enabled"] == group[1]["time_enabled"], "event group enabled times differ")
    for event in EVENTS:
        require(totals[event] == sum(r["count"] for r in raw if r["event"] == event), "raw and summary counts differ")
    return {**totals, "wall_seconds": seconds, "ipc": totals["instructions"] / totals["cpu-cycles"], "raw": raw}


def describe(values):
    median = statistics.median(values)
    return {"values": values, "median": median, "min": min(values), "max": max(values),
            "mad": statistics.median(abs(v - median) for v in values)}


def workload_points(manifest, root):
    workload = manifest.get("workload")
    if workload is None:
        return {count: {"argv": ["0x0", "0x0", "0x66", str(count), "7", "1", "2000"], "crc": crc}
                for count, crc in CRC.items()}
    require(workload["kind"] == "bench-suite", "unknown benchmark workload")
    kernel = workload["kernel"]
    require(kernel in {"int", "fp", "mem", "branch", "call"}, "unknown bench-suite kernel")
    points = {}
    for point in workload["points"]:
        scale = point["scale"]
        require(type(scale) is int and scale > 0 and scale not in points, "duplicate/invalid benchmark scale")
        digest = point["stdout_sha256"]
        require(re.fullmatch(r"[0-9a-f]{64}", digest) is not None, "invalid golden stdout SHA-256")
        require(any(a["path"] == point["stdout_path"] and a["sha256"] == digest for a in manifest["artifacts"]),
                "golden stdout must be a registered artifact")
        require(sha256(root / point["stdout_path"]) == digest, "golden stdout identity differs")
        points[scale] = {"argv": [kernel, str(scale)], "stdout_sha256": digest}
    require(len(points) == 2, "exactly two benchmark scales required")
    return points


def validate_sample(sample, manifest, root):
    directory = root / sample["directory"]
    arm = sample["arm"]
    count = sample["iterations"]
    require(type(count) is int, "work count must be an integer")
    point = workload_points(manifest, root)[count]
    environment = pairs((directory / "environment.txt").read_text())
    require(environment == sample["environment"], "environment differs from scheduled observation")
    require(not any(re.search(r"DUMP|PROF|DEBUG|TRACE|AUDIT|CENSUS|VERIFY", k) for k in environment), "diagnostic setting in timed point")
    if arm == "fex":
        require(not any(k.startswith("SVM_") for k in environment), "SVM environment in FEX arm")
        for key in ("CACHEOBJECTCODECOMPILATION", "AOTIRLOAD", "AOTIRCAPTURE", "AOTIRGENERATE", "BLOCKJITNAMING", "LIBRARYJITNAMING", "GLOBALJITNAMING"):
            require(environment.get("FEX_" + key) == "0", f"FEX {key} must be disabled")
        require(environment.get("FEX_HOSTFEATURES") == "disableavx" and environment.get("FEX_PORTABLE") == "1", "FEX feature/config isolation differs")
    else:
        require(not any(k.startswith("FEX_") for k in environment), "FEX environment in SVM arm")
        require(environment.get("SVM_FUNC_BASE") == {"function": "1", "block": "0"}[sample["mode"]], "wrong SVM mode")
        require(environment.get("SVM_JIT_CACHE") == "", "SVM disk cache must be disabled")
        for key in ("AVX", "BMI", "XSAVE"):
            require(environment.get("SVM_" + key) == "0", f"SVM {key} must be disabled")
    expected_argv = [manifest["arms"][arm]["device_path"], manifest["guest"]["device_path"], *point["argv"]]
    require(read_argv(directory / "argv.nul") == expected_argv, "guest command/workload differs")
    profiler = ["/system/bin/taskset", "10", manifest["simpleperf"]["device_path"], "stat", "--csv", "--verbose", "--cpu", "4",
                "--group", "cpu-cycles,instructions", "-o", sample["device_directory"] + "/pmu.csv",
                "/system/bin/sh", "-c", WRAPPER, "pmu-observation", sample["device_directory"], CHILD_WRAPPER, *expected_argv]
    require(read_argv(directory / "profiler-argv.nul") == profiler, "PMU scope/group/affinity command differs")
    require((directory / "profiler-exit-code.txt").read_text().strip() == "0", "nonzero profiler exit")
    require((directory / "target-exit-code.txt").read_text().strip() == "0", "nonzero target exit (independent of profiler status)")
    if "crc" in point:
        require((directory / "expected-crc.txt").read_text().strip() == point["crc"], "wrong CRC expectation")
        output = (directory / "stdout.txt").read_text()
        require(re.findall(r"\[0\]crcfinal\s*:\s*(0x[0-9a-fA-F]+)", output) == [point["crc"]], "CRC mismatch/missing/duplicate")
        require(re.findall(r"^Iterations\s*:\s*(\d+)\s*$", output, re.M) == [str(count)], "iteration output differs")
    else:
        require((directory / "expected-stdout-sha256.txt").read_text().strip() == point["stdout_sha256"], "wrong stdout expectation")
        require(sha256(directory / "stdout.txt") == point["stdout_sha256"], "benchmark stdout mismatch")
    expected_identities = {manifest[key]["device_path"]: manifest[key]["sha256"] for key in ("point_script", "simpleperf", "guest")}
    expected_identities[manifest["arms"][arm]["device_path"]] = manifest["arms"][arm]["sha256"]
    for when in ("before", "after"):
        require(identities((directory / f"identity-{when}.txt").read_text()) == expected_identities, f"{when} identity differs")
    stderr = (directory / "stderr.txt").read_text()
    pids = re.findall(r"^SVM_BENCH_PID=(\d+)$", stderr, re.M)
    require(len(pids) == 1, "missing/duplicate measured PID")
    pid = int(pids[0])
    require(re.findall(r"^SVM_BENCH_CHILD_PID=(\d+)$", stderr, re.M) == [str(pid)], "launcher PID differs from waited child")
    counter_pids = re.findall(r"^SVM_BENCH_COUNTER_PID=(\d+)$", stderr, re.M)
    require(len(counter_pids) == 1 and int(counter_pids[0]) > 0, "missing/duplicate counter target PID")
    masks = re.findall(r"pid " + str(pid) + r"'s (?:current )?affinity mask:\s*([0-9a-fA-F]+)", stderr)
    require(len(masks) == 1 and int(masks[0], 16) == 0x10, "measured target affinity differs")
    pmu = parse_pmu(directory / "pmu.csv")
    require(int(counter_pids[0]) in {r["tid"] for r in pmu["raw"]}, "PMU counter target PID differs")
    telemetry = {when: pairs((directory / f"device-{when}.txt").read_text()) for when in ("before", "after")}
    for state in telemetry.values():
        require(state["model"] == "NX729J" and state["cpu4_online"] == "1", "device/CPU changed")
        require(state["fingerprint"] and state["scaling_governor"] != "unavailable", "device identity/governor unavailable")
        for zone, name in ((31, "cpuss-0"), (35, "cpu-1-0"), (41, "cpu-1-6")):
            require(state[f"thermal_{zone}_type"] == name, "thermal sensor mapping changed")
            int(state[f"thermal_{zone}_temp"])
        for key in ("scaling_cur_freq", "scaling_min_freq", "scaling_max_freq", "battery_temp", "battery_capacity"):
            int(state[key])
    hashes = {p.name: sha256(p) for p in directory.iterdir() if p.is_file()}
    return {**sample, "valid": True, "pid": pid, "pmu": pmu, "telemetry": telemetry, "raw_sha256": hashes}


def compare(manifest, root):
    require(manifest["schema"] == 1 and manifest["serial"] == "bf1359ff", "unknown schema/device serial")
    require(manifest["repetitions"] >= 3 and set(manifest["arms"]) == {"baseline", "candidate", "fex"}, "incomplete arms/repetitions")
    for artifact in manifest["artifacts"]:
        require(sha256(root / artifact["path"]) == artifact["sha256"], f"artifact identity differs: {artifact['path']}")
    expected = {a["device_path"]: a["sha256"] for a in manifest["artifacts"] if "device_path" in a}
    require(manifest["identity_before"] == manifest["identity_after"] == expected, "batch deployment identity differs")
    points = workload_points(manifest, root)
    low_point, high_point = sorted(points)
    results, issues = [], []
    for sample in manifest["samples"]:
        try:
            results.append(validate_sample(sample, manifest, root))
        except (ValueError, KeyError, OSError, IndexError) as error:
            issues.append(f"{sample.get('id')}: {error}")
            results.append({**sample, "valid": False, "error": str(error)})
    keys = [(s["mode"], s["arm"], s["round"], s["iterations"]) for s in results]
    matrix = set(itertools.product(("function", "block"), manifest["arms"], range(manifest["repetitions"]), points))
    if len(keys) != len(matrix) or set(keys) != matrix:
        issues.append("missing/duplicate primary sample; complete Block/Function x three arms x rounds x two points required")
    for mode in ("function", "block"):
        environments = {json.dumps(s["environment"], sort_keys=True) for s in results if s["mode"] == mode and s["arm"] != "fex"}
        if len(environments) != 1:
            issues.append(f"{mode}: baseline/candidate environments differ")
    if len({json.dumps(s["environment"], sort_keys=True) for s in results if s["arm"] == "fex"}) != 1:
        issues.append("primary FEX environment changed between observations")
    all_states = [state for sample in results if sample["valid"] for state in sample["telemetry"].values()]
    for key in ("fingerprint", "scaling_governor", "scaling_min_freq", "scaling_max_freq", "battery_status", "usb_online"):
        if len({s[key] for s in all_states}) != 1:
            issues.append(f"device condition changed or unavailable: {key}")
    summaries = {}
    if not issues:
        lookup = {(s["mode"], s["arm"], s["round"], s["iterations"]): s["pmu"] for s in results}
        for mode, arm in itertools.product(("function", "block"), manifest["arms"]):
            entry = {}
            for metric in (*EVENTS, "wall_seconds"):
                low = [lookup[mode, arm, r, low_point][metric] for r in range(manifest["repetitions"])]
                high = [lookup[mode, arm, r, high_point][metric] for r in range(manifest["repetitions"])]
                slopes = [(hi - lo) / (high_point - low_point) for lo, hi in zip(low, high)]
                entry[metric] = {f"total_{low_point}": describe(low), f"total_{high_point}": describe(high), "slope": describe(slopes),
                                 "intercept": describe([lo - low_point * slope for lo, slope in zip(low, slopes)])}
                if any(v <= 0 for v in slopes):
                    issues.append(f"{mode}/{arm}/{metric}: nonpositive slope; two-point model is not usable")
                if any(v < 0 for v in entry[metric]["intercept"]["values"]):
                    entry[metric]["negative_intercept"] = True
            cycles = entry["cpu-cycles"]["slope"]["values"]
            entry["slope_ipc"] = describe([i / c for i, c in zip(entry["instructions"]["slope"]["values"], cycles)]) if all(c > 0 for c in cycles) else {"undefined": "nonpositive cycle slope"}
            summaries[f"{mode}/{arm}"] = entry
        for mode in ("function", "block"):
            for reference in ("baseline", "fex"):
                ratios = {}
                for metric, component in itertools.product((*EVENTS, "wall_seconds"), (f"total_{low_point}", "slope", "intercept")):
                    candidate = summaries[f"{mode}/candidate"][metric][component]["values"]
                    other = summaries[f"{mode}/{reference}"][metric][component]["values"]
                    ratios[f"{metric}/{component}"] = describe([c / b for c, b in zip(candidate, other)]) if all(b > 0 for b in other) else {"undefined": "nonpositive reference; raw estimates retained"}
                summaries[f"{mode}/candidate_over_{reference}"] = ratios
    auxiliary = []
    for sample in manifest.get("cold_service_samples", []):
        try:
            require(sample["arm"] == "fex", "cold-service observation must use FEX")
            auxiliary.append(validate_sample(sample, manifest, root))
        except (ValueError, KeyError, OSError, IndexError) as error:
            auxiliary.append({**sample, "valid": False, "error": str(error)})
    service = manifest.get("service", {})
    service_result = {"scope": service.get("scope"), "measured": False}
    if "pmu_path" in service:
        try:
            service_result.update(pmu=parse_pmu(root / service["pmu_path"]), measured=True)
        except (ValueError, KeyError, OSError, IndexError) as error:
            service_result["error"] = str(error)
    return {"measurement_valid": not issues, "issues": issues, "samples": results, "summary": summaries,
            "cold_service_samples": auxiliary, "service": service_result,
            "formal_FEX_gate_passed": False, "full_refactor_complete": False,
            "workload": manifest.get("workload", {"kind": "coremark", "points": [2000, 12000]}),
            "limits": ["Two-point intercept includes loader, runtime and compilation; it is not pure compiler cost.",
                       "FEX is an external reference with remaining CPUID differences; CRC/JIT map evidence does not prove complete capability equality.",
                       "Primary samples exclude the separately started FEXServer; service costs and cold-service scope must be reported separately.",
                       "Three repetitions describe observed spread, not a confidence interval; short runs are not official CoreMark scores."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = compare(json.loads(args.manifest.read_text()), args.manifest.resolve().parent)
    except (ValueError, KeyError, OSError, IndexError) as error:
        result = {"measurement_valid": False, "issues": [str(error)]}
    result["manifest_sha256"] = sha256(args.manifest)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result[k] for k in ("measurement_valid", "issues")}, indent=2))
    print(f"Full results: {args.output}")
    return 0 if result["measurement_valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
