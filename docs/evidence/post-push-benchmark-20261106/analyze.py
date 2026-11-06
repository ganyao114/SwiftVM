"""Analyze all preregistered samples without invoking the device."""

from pathlib import Path
from statistics import median
import datetime
import hashlib
import json
import math
import re
import subprocess
import tarfile

P = Path(__file__).resolve().parent
R = Path("/Volumes/Data2/SwiftVM-submit-20261104")
ARMS = ["block", "function", "fex"]
PMU = ["cpu-cycles:u", "instructions:u", "branch-misses:u"]
FEX_GROUPS = {
    "formation_and_ir": ["raw_decode", "builder", "generate_ir"],
    "ir_optimization": ["pass_manager", "x87", "dfe", "validation", "other_pass"],
    "allocation_and_encoding": ["ra", "backend", "emit"],
    "transfer_and_publication": ["tail_copy_cache", "publish"],
    "logical_work_buffer_release": ["logical_release"],
    "other": ["compile_block", "compile_code"],
}
WORK = [
    "compile_attempts",
    "function_attempts",
    "block_attempts",
    "fallbacks",
    "code_units_published",
    "emitted_guest_instructions",
    "emitted_guest_bytes",
    "ir_nodes",
    "operand_words",
    "native_bytes",
    "spill_slots",
    "max_frame_bytes",
    "fault_sites",
    "static_links",
    "peak_slabs",
    "peak_code_virtual_bytes",
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, label, positive=False):
    require(type(value) in (int, float) and math.isfinite(value), label + ": nonfinite")
    require(value > 0 if positive else value >= 0, label + ": invalid sign")
    return value


def read(name):
    return json.loads((P / name).read_text())


def save(name, value):
    (P / name).write_text(json.dumps(value, indent=2) + "\n")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(values):
    return dict(median=median(values), min=min(values), max=max(values), samples=values)


def exclusive(row):
    rec = row["record"]
    total = number(row["compile_excluding_byte_ns"], "compile total", True)
    extra = {}
    if row["arm"] == "fex":
        require(
            rec.get("qualified") is True
            and rec.get("closed") is True
            and rec.get("clock_supported") is True,
            "FEX probe not qualified/closed",
        )
        for key in [
            "stack_errors",
            "arithmetic_errors",
            "unclassified_pass_runs",
            "reentries",
        ]:
            require(rec[key] == 0, "FEX probe error: " + key)
        stages = {x["name"]: x for x in rec["stages"]}
        require(len(stages) == len(rec["stages"]), "duplicate FEX stage")
        for s in stages.values():
            for key in ["ticks", "children_ticks", "remaining_ticks"]:
                number(s[key], "FEX " + key)
            require(
                s["ticks"] - s["children_ticks"] == s["remaining_ticks"],
                "FEX exclusive stage mismatch",
            )
        require(
            sum(s["remaining_ticks"] for s in stages.values())
            == rec["root_ticks"]
            == rec["exclusive_ticks"],
            "FEX whole tick ledger does not close",
        )
        byte = stages["byte_decode"]
        require(byte["children_ticks"] == 0, "byte decode scope includes nested work")
        freq = number(rec["cntfrq_hz"], "FEX cntfrq", True)
        ticks = {
            k: sum(stages[n]["remaining_ticks"] for n in names)
            for k, names in FEX_GROUPS.items()
        }
        require(
            sum(ticks.values()) == rec["root_ticks"] - byte["ticks"],
            "FEX grouped ledger does not close",
        )
        parts = {k: v * 1000 / freq for k, v in ticks.items()}
        extra = {
            "exclusive_ticks": ticks,
            "cntfrq_hz": freq,
            "root_calls": rec["root_calls"],
            "byte_decode_calls": byte["calls"],
            "exact_tick_closure": True,
        }
    else:
        require(
            rec["runtime_completed"] is True
            and rec["exit_code"] == (101 if row["case"] == "func_tests" else 0),
            "SVM compile launch did not complete",
        )
        require(
            rec["compile_without_byte_decode_ns"]
            == total
            == rec["compile_ns"] - rec["compile_decode_ns"],
            "SVM byte exclusion mismatch",
        )
        parts = {
            "formation_and_ir": (rec["frontend_ns"] - rec["compile_decode_ns"]) / 1e6,
            "ir_optimization": rec["optimize_ns"] / 1e6,
            "allocation_and_encoding": rec["backend_ns"] / 1e6,
            "transfer_and_publication": rec["publication_ns"] / 1e6,
            "native_owner_release": rec["code_release_ns"] / 1e6,
        }
        parts["other"] = total / 1e6 - sum(parts.values())
        extra = {"fault_service_ms_in_other": rec["fault_service_ns"] / 1e6}
    require(
        all(math.isfinite(v) and v >= -1e-12 for v in parts.values()),
        "negative/nonfinite exclusive component",
    )
    error = sum(parts.values()) - total / 1e6
    require(abs(error) < 1e-8, "exclusive sum differs from total")
    return {
        "round": row["round"],
        "prefix": row["prefix"],
        "total_ms": total / 1e6,
        "exclusive_ms": parts,
        "closure_error_ms": error,
        **extra,
    }


def main():
    plan, identity, source = (
        read("plan.json"),
        read("identity.json"),
        read("source-manifest.json"),
    )
    require(sha(P / "plan.json") == identity["measurement_plan_sha256"], "plan drift")
    require(sha(P / "run.py") == plan["harness_sha256"], "harness drift")
    require(
        sha(P / "source-manifest.json") == plan["source_manifest_sha256"],
        "source manifest drift",
    )
    require(
        subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=R, text=True).strip()
        == plan["subject_commit"],
        "subject changed",
    )
    digest = hashlib.sha256()
    with tarfile.open(P / "selected-sources.tar.gz") as archive:
        for item in sorted(source["files"], key=lambda x: x["path"]):
            name, expected = item["path"], item["sha256"]
            require(sha(R / name) == expected, "current source drift: " + name)
            require(
                hashlib.sha256(archive.extractfile(name).read()).hexdigest()
                == expected,
                "frozen source drift: " + name,
            )
            digest.update((name + "\0" + expected + "\n").encode())
    # The literals above represent actual NUL and LF in the digest protocol.
    expected_digest = hashlib.sha256(
        "".join(
            x["path"] + "\0" + x["sha256"] + "\n"
            for x in sorted(source["files"], key=lambda x: x["path"])
        ).encode()
    ).hexdigest()
    require(
        expected_digest
        == digest.hexdigest()
        == source["digest"]
        == plan["source_digest"],
        "source digest mismatch",
    )
    cleanup = read("phone-cleanup-receipt.json")
    require(
        cleanup["remaining_bytes"] == 0
        and not cleanup["active_processes"]
        and not cleanup["open_files"]
        and not cleanup["errors"],
        "device cleanup incomplete",
    )
    inputs = read("inputs.json")
    for phase in ("before", "after"):
        actual = {
            line.split()[1].removeprefix(plan["namespace"] + "/"): line.split()[0]
            for line in (P / ("remote-identities-" + phase + ".stdout"))
            .read_text()
            .splitlines()
        }
        require(
            actual == {name: item["sha256"] for name, item in inputs.items()},
            "device input identity drift: " + phase,
        )
    require(inputs["candidate"]["sha256"] == plan["runner_sha256"], "runner mismatch")
    cases = read("cases.json")
    case_by_name = {case["name"]: case for case in cases}
    expected_grid = {(c["name"], a, n) for c in cases for a in ARMS for n in range(5)}
    runtime, compiled = read("runtime-records.json"), read("compile-records.json")
    for kind, rows in [("runtime", runtime), ("compile", compiled)]:
        require(
            len(rows) == 120
            and {(r["case"], r["arm"], r["round"]) for r in rows} == expected_grid,
            kind + ": incomplete grid",
        )
        require(
            all(row["valid"] for row in rows),
            kind + ": invalid samples retained; see raw records",
        )
        for row in rows:
            stdout = (P / (row["prefix"] + ".stdout")).read_text()
            case = case_by_name[row["case"]]
            if "checksum" in case:
                found = re.search(
                    r"^" + case["name"] + r"\s+(0x[0-9a-f]+)\s*$", stdout, re.M
                )
                require(
                    bool(found and found[1] == case["checksum"]),
                    row["prefix"] + ": checksum",
                )
            if row["case"] == "coremark":
                crc = "0xd340" if kind == "runtime" else "0x4983"
                require(
                    re.search(r"crcfinal\s*:\s*" + crc, stdout) is not None,
                    row["prefix"] + ": CRC",
                )
            if kind == "runtime":
                require(
                    row["exit"] == case["exit"] and row["cpu_allowed_list"] == "4",
                    row["prefix"] + ": exit/affinity",
                )
                require(set(row["pmu"]) == set(PMU), row["prefix"] + ": missing PMU")
                if row["case"] == "coremark":
                    require(
                        row["seconds"] >= 10
                        and "Correct operation validated." in stdout,
                        row["prefix"] + ": duration",
                    )
    report, breakdown, resources = [], [], []
    for case in cases:
        name = case["name"]
        arms, parts, work = {}, {}, {}
        for arm in ARMS:
            rr = [r for r in runtime if r["case"] == name and r["arm"] == arm]
            cc = [r for r in compiled if r["case"] == name and r["arm"] == arm]
            arms[arm] = dict(
                runtime_ms=stats([r["seconds"] * 1000 for r in rr]),
                whole_child_ms=stats([r["whole_child_seconds"] * 1000 for r in rr]),
                compile_ms=stats([r["compile_excluding_byte_ns"] / 1e6 for r in cc]),
                pmu={event: stats([r["pmu"][event] for r in rr]) for event in PMU},
            )
            if name == "coremark":
                arms[arm]["iterations_per_second"] = stats(
                    [r["coremark_iterations_per_second"] for r in rr]
                )
            all_parts = [exclusive(r) for r in cc]
            selected = sorted(all_parts, key=lambda r: (r["total_ms"], r["round"]))[2]
            parts[arm] = dict(selected_median_sample=selected, all_samples=all_parts)
            if arm != "fex":
                work[arm] = {
                    key: sorted({r["record"][key] for r in cc}) for key in WORK
                }
        ratios = {}
        for scope in ("block", "function"):
            candidate, fex = arms[scope], arms["fex"]
            value = dict(
                runtime=candidate["runtime_ms"]["median"] / fex["runtime_ms"]["median"],
                compile=candidate["compile_ms"]["median"] / fex["compile_ms"]["median"],
                cycles=candidate["pmu"]["cpu-cycles:u"]["median"]
                / fex["pmu"]["cpu-cycles:u"]["median"],
                instructions=candidate["pmu"]["instructions:u"]["median"]
                / fex["pmu"]["instructions:u"]["median"],
                branch_misses=candidate["pmu"]["branch-misses:u"]["median"]
                / fex["pmu"]["branch-misses:u"]["median"],
            )
            value["gates"] = dict(
                time=value["runtime"] <= 1.1,
                cycles=value["cycles"] <= 1.1,
                compile=value["compile"] <= 2.0,
            )
            ratios[scope] = value
        report.append(dict(case=name, arms=arms, ratios=ratios))
        breakdown.append(dict(case=name, arms=parts))
        resources.append(dict(case=name, arms=work))
    acceptance = [
        dict(
            case=row["case"],
            scope=scope,
            metric=metric,
            ratio=row["ratios"][scope]["runtime" if metric == "time" else metric],
            passed=passed,
        )
        for row in report
        for scope in ("block", "function")
        for metric, passed in row["ratios"][scope]["gates"].items()
    ]
    policies = {
        path.name: path.read_text().split()
        for path in sorted(P.glob("*policy*.stdout"))
    }
    require(
        all(value[:2] == ["614400", "1171200"] for value in policies.values()),
        "frequency policy changed",
    )
    summary = dict(
        subject_commit=plan["subject_commit"],
        source_digest=source["digest"],
        source_files=len(source["files"]),
        runtime_samples=len(runtime),
        compile_samples=len(compiled),
        results=report,
        acceptance=acceptance,
        gates_passed=sum(r["passed"] for r in acceptance),
        gates_total=len(acceptance),
        policy_observations=policies,
        validation=dict(
            source_current_and_archive=True,
            device_input_hashes_before_after=True,
            complete_fixed_sample_grid=True,
            compile_exclusive_parts_closed=True,
            affinity_live_pid_samples=sum(r["live_pid_verified"] for r in runtime),
            affinity_inheritance_samples=sum(
                not r["live_pid_verified"] for r in runtime
            ),
        ),
        limits=plan["known_limits"],
    )
    save("summary.json", summary)
    save("compile-breakdown.json", breakdown)
    save("resources.json", resources)
    for row in report:
        print(
            row["case"],
            "runtime ms B/F/FEX:",
            *(round(row["arms"][a]["runtime_ms"]["median"], 3) for a in ARMS),
            "compile ms:",
            *(round(row["arms"][a]["compile_ms"]["median"], 6) for a in ARMS),
        )
        print("ratios:", json.dumps(row["ratios"]))
    print("Gates:", summary["gates_passed"], "/", summary["gates_total"])


if __name__ == "__main__":
    main()
