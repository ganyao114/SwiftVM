#!/usr/bin/env python3
"""Measure already deployed fixtures; preserve every sample and its launcher log."""

import argparse
import hashlib
import json
import shlex
import statistics
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--namespace", default="/data/local/tmp/swiftvm-indirect-20261107")
    parser.add_argument("--device", default="bf1359ff")
    args = parser.parse_args()
    root, namespace = args.results, args.namespace
    adb = ["adb", "-s", args.device]
    plan = json.loads((root / "plan.json").read_text())
    manifest_path = root / "guest/manifest.json"
    manifest = json.loads(manifest_path.read_text())
    assert hashlib.sha256(manifest_path.read_bytes()).hexdigest() == plan["guest_manifest_sha256"]
    environment = json.loads((root / "environment.json").read_text())
    commands, records = [], []
    raw = root / "samples"
    raw.mkdir()

    def save(name, value):
        (root / name).write_text(json.dumps(value, indent=2) + "\n")

    def remote(label, command, timeout=50):
        argv = adb + ["shell", "su -c " + shlex.quote(shlex.join(command))]
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        (raw / (label + ".stdout")).write_text(result.stdout)
        (raw / (label + ".stderr")).write_text(result.stderr)
        commands.append({"label": label, "argv": argv, "exit": result.returncode})
        save("measurement-commands.json", commands)
        if result.returncode:
            raise RuntimeError(f"{label}: exit {result.returncode}: {result.stderr[-1200:]}")
        return result.stdout

    policy_files = [
        "/sys/devices/system/cpu/cpu4/cpufreq/" + item
        for item in ("scaling_min_freq", "scaling_max_freq", "scaling_cur_freq")
    ] + ["/sys/class/power_supply/battery/temp", "/sys/class/thermal/thermal_zone31/temp"]
    before = remote("policy-before", ["cat", *policy_files]).splitlines()
    save("measurement-policy-before.json", before)
    engines = plan["engines"]
    for repetition in range(plan["repetitions_per_case_engine"]):
        for index, case in enumerate(manifest["cases"]):
            offset = (repetition + index) % len(engines)
            for engine in engines[offset:] + engines[:offset]:
                label = f"r{repetition}-{case['name']}-{engine}"
                prefix = namespace + "/samples/" + label
                if engine == "fex":
                    launcher = ["/system/bin/env", "-i"] + [
                        key + "=" + value for key, value in environment.items()
                    ] + [namespace + "/FEXLoader"]
                else:
                    launcher = [
                        "/system/bin/env", "-i", "PATH=/system/bin", namespace + "/svm",
                        "--scope", engine,
                    ]
                remote(label, [namespace + "/pmu-region", "4", prefix, *launcher,
                               namespace + "/guests/" + Path(case["binary"]["path"]).name])
                result = json.loads(remote(label + "-result", ["cat", prefix + ".json"]))
                stderr = remote(label + "-launcher", ["cat", prefix + ".stderr"])
                assert result["checksum"] == case["expected_checksum"], (label, result)
                assert result["exit_code"] == 0
                assert result["time_running_ns"] == result["time_enabled_ns"] > 0
                stats = None
                if engine != "fex":
                    stats = json.loads(next(line for line in stderr.splitlines()
                                            if line.startswith("{")))
                    assert stats["runtime_completed"] and not stats["interpreted_steps"]
                    assert not stats["dispatch_counts_available"]
                row = {
                    "label": label, "case": case["name"], "engine": engine,
                    "repetition": repetition, "iterations": case["iterations"],
                    "measurement": result, "runtime_stats": stats,
                    "per_iteration": {key: result[key] / case["iterations"]
                                      for key in ("guest_ns", "cycles", "instructions", "branch_misses")},
                }
                records.append(row)
                save("records.json", records)
                if len(records) % 6 == 0:
                    print(f"measured {len(records)} samples: {label}", flush=True)
        policy = remote(f"policy-round-{repetition}", ["cat", *policy_files]).splitlines()
        assert policy[:2] == before[:2], (before, policy)
    summary = []
    for case in manifest["cases"]:
        metrics = {}
        for engine in engines:
            selected = [row for row in records if row["case"] == case["name"]
                        and row["engine"] == engine]
            metrics[engine] = {
                key: statistics.median(row["per_iteration"][key] for row in selected)
                for key in ("guest_ns", "cycles", "instructions", "branch_misses")
            }
        summary.append({
            "case": case["name"], "branch": case["branch"], "direct": case["direct"],
            "targets": case["targets"], "stride": case["stride"], "metrics": metrics,
            "svm_over_fex": {engine: {key: metrics[engine][key] / metrics["fex"][key]
                                      for key in ("guest_ns", "cycles", "instructions")}
                             for engine in ("block", "function")},
        })
    save("comparison.json", summary)
    save("measurement-summary.json", {
        "all_passed": True, "samples": len(records), "cases": len(manifest["cases"]),
        "repetitions": plan["repetitions_per_case_engine"],
        "policy_before": before, "policy_after": policy,
        "fixed_samples_no_fastest_selection": True,
    })
    for row in summary:
        print(row["case"], {engine: round(value["guest_ns"], 3)
                            for engine, value in row["metrics"].items()}, flush=True)


if __name__ == "__main__":
    main()
