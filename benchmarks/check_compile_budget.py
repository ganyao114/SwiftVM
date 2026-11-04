#!/usr/bin/env python3
"""Validate compile budgets on one monotonic wall axis; schema tests are not measurements."""
import argparse
import copy
import hashlib
import json
import math
import sys
from decimal import Decimal
from pathlib import Path

PERCENT = {"A": 50, "B": 25, "C": 90, "D": 25, "other": 10}
POLICY = {key: value / 100 for key, value in PERCENT.items()}
BASIS = "compile_excluding_decode"

class Missing(Exception):
    pass

def need(ok, label):
    if not ok:
        raise Missing(label)

def named(value):
    return isinstance(value, str) and bool(value.strip())

def digest(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)

def union(intervals):
    merged = []
    for begin, end in sorted(intervals):
        if begin == end:
            continue
        if merged and begin <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([begin, end])
    return merged

def length(intervals):
    return sum(end - begin for begin, end in intervals)

def wall_intervals(record, clock, label, allow_empty=False):
    need(record.get("wall_clock", clock) == clock, label + " wall-clock domain")
    rows = record.get("wall_intervals_ns")
    need(isinstance(rows, list), label + " real wall intervals")
    for row in rows:
        need(isinstance(row, list) and len(row) == 2 and all(type(x) is int for x in row), label + " integer ns pair")
        need(0 <= row[0] <= row[1], label + " nonnegative ordered interval")
    merged = union(rows)
    duration = length(merged)
    for key in ("inclusive_ms", "inclusive_compile_ms"):
        if key in record:
            value = record[key]
            need(type(value) in (int, float) and math.isfinite(value) and value >= 0, label + " finite declared duration")
            need(Decimal(str(value)) * 1_000_000 == duration, label + " duration disagrees with wall union")
    if not duration and not allow_empty:
        need(record.get("inclusive_ms") == 0 and type(record.get("inclusive_ms")) in (int, float), label + " explicit zero work")
    return merged

def compile_window(record, clock, label):
    need(record.get("decode_exclusion_kind") == "byte_decode_only", label + " exclude only byte decode, keep all IR work")
    charged = wall_intervals(record, clock, label)
    decoded = wall_intervals({"wall_intervals_ns": record.get("excluded_decode_intervals_ns")},
                             clock, label + " excluded decode", allow_empty=True)
    need(length(union(charged + decoded)) == length(charged) + length(decoded),
         label + " decode overlaps charged compile time")
    return charged, length(decoded)

def contained(inner, outer):
    cursor = 0
    for begin, end in inner:
        while cursor < len(outer) and outer[cursor][1] <= begin:
            cursor += 1
        if cursor == len(outer) or not (outer[cursor][0] <= begin and end <= outer[cursor][1]):
            return False
    return True

def ledger_counts(rows, scope):
    need(isinstance(rows, list) and bool(rows), scope + " complete attempt ledger")
    ids = set()
    counts = dict(attempted=len(rows), published=0, failed=0, abandoned=0, N=0, E=0, words=0)
    for row in rows:
        need(row.get("case_scope") == scope and named(row.get("id")) and row["id"] not in ids, scope + " request scope/id")
        ids.add(row["id"])
        need(row.get("outcome") in ("published", "failed") and type(row.get("abandoned")) is bool, "request outcome")
        need(row.get("accounted") is True, "failed/abandoned cost accounted")
        counts[row["outcome"]] += 1
        counts["abandoned"] += row["abandoned"]
        need(not row["abandoned"] or row["outcome"] == "failed", "abandoned outcome")
        for key in ("N", "E", "words"):
            need(type(row.get(key)) is int and row[key] >= 0, "complete prefix " + key)
            counts[key] += row[key]
    sequence = hashlib.sha256(json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return counts, sequence

def evaluate(manifest, read, stage="full"):
    try:
        need(stage in ("A", "full"), "explicit stage")
        need(manifest.get("comparison_basis") == BASIS, "explicit compile-excluding-decode comparison basis")
        need(manifest.get("policy_fex_multipliers") == POLICY, "fixed policy")
        workload = manifest["workload"]
        need(workload.get("kind") == "application" and workload.get("fixed") is True, "real fixed application")
        need(digest(workload.get("application_sha256")) and digest(workload.get("fingerprint_sha256")), "application identity")
        need(named(manifest.get("round_id")) and named(manifest.get("timer_semantics")), "round/timer semantics")
        environment, clock = manifest["environment"], manifest["wall_clock"]
        need(named(environment.get("device_serial")) and named(environment.get("cpu_affinity")) and digest(environment.get("policy_identity_sha256")), "environment identity")
        need(clock.get("kind") == "monotonic_wall" and clock.get("unit") == "ns" and named(clock.get("domain")), "monotonic wall clock, not CPU time")
        details, violations = {}, []
        for scope in ("block", "function"):
            counts, sequence = ledger_counts(manifest["request_attempts"][scope], scope)
            records = {side: read(manifest["measurements"][scope][side]) for side in ("candidate", "fex")}
            for side, record in records.items():
                identity = manifest["fixed_" + side + "_identity"]
                need(digest(identity.get("source_digest")) and digest(identity.get("binary_sha256")), side + " fixed identity")
                need(record.get("source_digest") == identity["source_digest"] and record.get("binary_sha256") == identity["binary_sha256"], side + " measured identity")
                need(record.get("scope") == scope and record.get("round_id") == manifest["round_id"], side + " scope/round")
                need(record.get("comparison_basis") == BASIS, side + " comparison basis mismatch")
                need(record.get("workload_fingerprint_sha256") == workload["fingerprint_sha256"] and record.get("application_sha256") == workload["application_sha256"], side + " workload")
                need(record.get("fresh_process") is True and record.get("compile_cache_empty") is True and record.get("coverage_complete") is True, side + " cold complete coverage")
                need(record.get("environment") == environment and record.get("timer_semantics") == manifest["timer_semantics"] and record.get("wall_clock") == clock, side + " environment/clock/semantics")
                need(record.get("measurement_kind") == "application_compile", side + " real compile endpoint, not writer replay")
                need(record.get("application_workload") is True and record.get("synthetic") is False and record.get("partial_compile_coverage") is False, side + " synthetic/partial post-decode work ineligible")
            candidate, reference = records["candidate"], records["fex"]
            need(reference.get("all_compile_threads_accounted") is True and reference.get("measurement_span") == "full", "complete FEX threads/span")
            endpoint = manifest["fex_endpoints"]
            need(named(endpoint.get("start")) and named(endpoint.get("end")) and reference.get("endpoints") == endpoint, "FEX named endpoints")
            reference_window, fex_decode_ns = compile_window(reference, clock, "FEX")
            fex_ns = length(reference_window)
            need(fex_ns > 0, "positive FEX complete wall time")
            span = candidate.get("measurement_span")
            need(span in ("A", "full") and (stage != "full" or span == "full"), "candidate measured span")
            window, candidate_decode_ns = compile_window(candidate, clock, "candidate")
            total_ns = length(window)
            need(total_ns > 0 and candidate.get("failure_costs_included") is True, "positive complete window with failure costs")
            actual = candidate.get("requests")
            need(actual == counts and all(type(actual[k]) is int for k in counts) and candidate.get("request_sequence_sha256") == sequence, "actual request/N/E/prefix ledger")
            measured, parts = {}, []
            for name in (("A",) if stage == "A" else ("A", "B", "C", "D")):
                phase, endpoint = candidate["phases"][name], manifest["stage_endpoints"][name]
                need(named(endpoint.get("start")) and named(endpoint.get("end")) and phase.get("endpoints") == endpoint, name + " named endpoints")
                intervals = wall_intervals(phase, clock, name)
                need(contained(intervals, window), name + " outside complete wall window")
                measured[name] = length(intervals)
                parts.extend((begin, end, name) for begin, end in intervals)
                if span == "A":
                    need(intervals == window, "A-only window must be entirely charged to A")
            if stage == "full":
                end = -1
                for begin, finish, name in sorted(parts):
                    need(begin >= end, "cross-phase wall overlap at " + name)
                    end = finish
                covered = union((begin, finish) for begin, finish, _ in parts)
                measured["other"] = total_ns - length(covered)
            bad = [name for name, value in measured.items() if value * 100 > fex_ns * PERCENT[name]]
            if stage == "full" and total_ns > 2 * fex_ns:
                bad.append("total")
            violations.extend(scope + ":" + name for name in bad)
            details[scope] = {"measurement_span": span, "compile_without_decode_ns": total_ns, "fex_without_decode_ns": fex_ns,
                              "excluded_decode_ns": {"candidate": candidate_decode_ns, "fex": fex_decode_ns}, "measured_ns": measured, "over_budget": bad}
        status = "OVER_BUDGET" if violations else ("PASS_A_BUDGET_ONLY" if stage == "A" else "PASS")
        return {"status": status, "stage": stage, "comparison_basis": BASIS, "policy": POLICY, "details": details,
                "compile_excluding_decode_2x_cost_gate_passed": stage == "full" and not violations, "quality_acceptance": False}, 1 if violations else 0
    except (Missing, KeyError, TypeError, AttributeError, ValueError, OSError, ArithmeticError) as error:
        return {"status": "MISSING", "reason": str(error), "policy": POLICY, "quality_acceptance": False}, 2

def fiction():
    """In-memory schema fixtures only; nothing here describes a measured program."""
    sha = "a" * 64
    clock = {"kind": "monotonic_wall", "unit": "ns", "domain": "schema-only-device/CLOCK_MONOTONIC"}
    environment = {"device_serial": "schema-only-device", "cpu_affinity": "cpu4", "policy_identity_sha256": sha}
    endpoints = {name: {"start": name + ".enter", "end": name + ".leave"} for name in ("A", "B", "C", "D")}
    manifest = {"comparison_basis": BASIS, "policy_fex_multipliers": POLICY, "round_id": "schema-fiction", "timer_semantics": "compile-excluding-decode-wall-union", "environment": environment, "wall_clock": clock,
                "workload": {"kind": "application", "fixed": True, "application_sha256": sha, "fingerprint_sha256": sha},
                "fixed_candidate_identity": {"source_digest": sha, "binary_sha256": sha}, "fixed_fex_identity": {"source_digest": sha, "binary_sha256": sha},
                "fex_endpoints": {"start": "compile.enter", "end": "compile.leave"}, "stage_endpoints": endpoints, "request_attempts": {}, "measurements": {}}
    records = {}
    for scope in ("block", "function"):
        rows = [{"case_scope": scope, "id": "p", "outcome": "published", "abandoned": False, "accounted": True, "N": 10, "E": 8, "words": 16},
                {"case_scope": scope, "id": "f", "outcome": "failed", "abandoned": True, "accounted": True, "N": 1, "E": 0, "words": 1}]
        manifest["request_attempts"][scope] = rows
        manifest["measurements"][scope] = {"candidate": "c-" + scope, "fex": "f-" + scope}
        common = {"scope": scope, "round_id": manifest["round_id"], "timer_semantics": manifest["timer_semantics"], "environment": environment, "wall_clock": clock,
                  "comparison_basis": BASIS, "decode_exclusion_kind": "byte_decode_only", "excluded_decode_intervals_ns": [],
                  "workload_fingerprint_sha256": sha, "application_sha256": sha, "source_digest": sha, "binary_sha256": sha,
                  "fresh_process": True, "compile_cache_empty": True, "coverage_complete": True, "measurement_kind": "application_compile", "application_workload": True, "synthetic": False, "partial_compile_coverage": False}
        records["f-" + scope] = common | {"measurement_span": "full", "all_compile_threads_accounted": True, "endpoints": manifest["fex_endpoints"], "wall_intervals_ns": [[0, 100_000_000]], "inclusive_compile_ms": 100}
        _, sequence = ledger_counts(rows, scope)
        phases = {name: {"endpoints": endpoints[name], "wall_intervals_ns": [[begin * 1_000_000, end * 1_000_000]], "inclusive_ms": end - begin}
                  for name, begin, end in (("A", 0, 40), ("B", 40, 60), ("C", 60, 140), ("D", 140, 160))}
        records["c-" + scope] = common | {"measurement_span": "full", "wall_intervals_ns": [[0, 165_000_000]], "inclusive_compile_ms": 165,
            "request_sequence_sha256": sequence, "requests": dict(attempted=2, published=1, failed=1, abandoned=1, N=11, E=8, words=17), "failure_costs_included": True, "phases": phases}
    return manifest, records

def self_test():
    manifest, records = fiction()
    def check(m, r, expected, stage="full"):
        result, _ = evaluate(m, r.__getitem__, stage)
        assert result["status"] == expected, result
    check(manifest, records, "PASS")
    only_a = copy.deepcopy(records)
    for scope in ("block", "function"):
        row = only_a["c-" + scope]
        row.update(measurement_span="A", wall_intervals_ns=[[0, 40_000_000]], inclusive_compile_ms=40, phases={"A": row["phases"]["A"]})
    check(manifest, only_a, "PASS_A_BUDGET_ONLY", "A")
    check(manifest, only_a, "MISSING")
    check(manifest, records, "PASS_A_BUDGET_ONLY", "A")
    # Only pure decode is deferred, symmetrically. The measured windows already
    # exclude it; do not subtract guessed decoder durations from old totals.
    for side in ("c-block", "f-block"):
        bad = copy.deepcopy(records); bad[side]["comparison_basis"] = "compile_including_decode"
        check(manifest, bad, "MISSING")
        bad = copy.deepcopy(records); del bad[side]["excluded_decode_intervals_ns"]
        check(manifest, bad, "MISSING")
        bad = copy.deepcopy(records); bad[side]["excluded_decode_intervals_ns"] = [[0, 1]]
        check(manifest, bad, "MISSING")
        bad = copy.deepcopy(records); bad[side]["decode_exclusion_kind"] = "entire_frontend"
        check(manifest, bad, "MISSING")
        okay = copy.deepcopy(records); okay[side]["partial_ISA"] = True
        check(manifest, okay, "PASS")  # Byte-decoder coverage is outside this metric.
    old = copy.deepcopy(manifest); del old["comparison_basis"]
    check(old, records, "MISSING")
    okay = copy.deepcopy(records)
    okay["f-block"].update(wall_intervals_ns=[[0, 30_000_000], [50_000_000, 120_000_000]],
                           excluded_decode_intervals_ns=[[30_000_000, 50_000_000]])
    check(manifest, okay, "PASS")  # FEX charged time stays 100ms, pure decode is 20ms.
    for side in ("c-block", "f-block"):
        for key, value in (("application_workload", False), ("synthetic", True), ("partial_compile_coverage", True), ("measurement_kind", "birth_ir_writer_replay")):
            bad = copy.deepcopy(records); bad[side][key] = value
            check(manifest, bad, "MISSING"); check(manifest, bad, "MISSING", "A")
    bad = copy.deepcopy(records)
    for name, begin, end in (("A", 0, 40), ("B", 10, 30), ("C", 30, 110), ("D", 110, 130)):
        bad["c-block"]["phases"][name].update(wall_intervals_ns=[[begin * 1_000_000, end * 1_000_000]], inclusive_ms=end - begin)
    bad["c-block"]["intervals_nonoverlapping"] = True
    check(manifest, bad, "MISSING")  # Old gate called other 5ms; actual other is 35ms.
    for begin, end in ((10, 30), (160, 180)):
        bad = copy.deepcopy(records)
        bad["c-block"]["phases"]["B"].update(wall_intervals_ns=[[begin * 1_000_000, end * 1_000_000]], inclusive_ms=end - begin)
        bad["c-block"]["intervals_nonoverlapping"] = True
        check(manifest, bad, "MISSING")
    for value in (39, -1, float("nan")):
        bad = copy.deepcopy(records); bad["c-block"]["phases"]["A"]["inclusive_ms"] = value; check(manifest, bad, "MISSING")
    bad = copy.deepcopy(records); bad["c-block"]["phases"]["A"]["wall_intervals_ns"] = [[0, float("nan")]]; check(manifest, bad, "MISSING")
    bad = copy.deepcopy(records); del bad["c-block"]["wall_intervals_ns"]; check(manifest, bad, "MISSING")
    bad = copy.deepcopy(records); bad["c-block"]["inclusive_compile_ms"] = 166; check(manifest, bad, "MISSING")
    bad = copy.deepcopy(only_a); bad["c-block"].update(wall_intervals_ns=[[0, 41_000_000]], inclusive_compile_ms=41); check(manifest, bad, "MISSING", "A")
    bad = copy.deepcopy(records); bad["f-block"]["wall_clock"] = {"kind": "thread_cpu", "unit": "ns", "domain": "cpu"}; check(manifest, bad, "MISSING")
    bad = copy.deepcopy(records); bad["f-block"]["wall_intervals_ns"] = [[0, 60_000_000], [40_000_000, 100_000_000]]; check(manifest, bad, "PASS")
    bad["f-block"]["inclusive_compile_ms"] = 120; check(manifest, bad, "MISSING")
    bad = copy.deepcopy(records)
    for scope in ("block", "function"):
        row = bad["c-" + scope]; row.update(wall_intervals_ns=[[0, 145_000_000]], inclusive_compile_ms=145)
        for name, intervals, duration in (("B", [], 0), ("C", [[40_000_000, 120_000_000]], 80), ("D", [[120_000_000, 140_000_000]], 20)):
            row["phases"][name].update(wall_intervals_ns=intervals, inclusive_ms=duration)
    check(manifest, bad, "PASS")
    bad = copy.deepcopy(manifest); bad["request_attempts"]["block"] = None; check(bad, records, "MISSING")
    bad = copy.deepcopy(manifest); bad["policy_fex_multipliers"]["A"] = 9; check(bad, records, "MISSING")
    bad = copy.deepcopy(records)
    bad["c-block"]["phases"]["A"].update(wall_intervals_ns=[[0, 60_000_000]], inclusive_ms=60)
    check(manifest, bad, "OVER_BUDGET", "A")
    print(json.dumps({"schema_self_test": "passed", "not_measurement": True, "cases": ["fictional_A_full", "both_record_eligibility", "without_decode_basis", "decode_exclusion_disjoint", "decoder_coverage_deferred", "writer_replay_kind", "overlap_despite_boolean", "outside_window", "duration_mismatch", "NaN_missing", "wall_not_CPU_sum", "explicit_zero_stage", "policy_ledger", "overspending"]}))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", nargs="?")
    parser.add_argument("--stage", choices=("A", "full"), default="full")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        try:
            need(bool(args.manifest), "manifest path")
            path = Path(args.manifest); manifest = json.loads(path.read_text())
            def read(spec):
                need(isinstance(spec, dict) and digest(spec.get("sha256")) and named(spec.get("path")), "artifact path/hash")
                data = (path.parent / spec["path"]).read_bytes()
                need(hashlib.sha256(data).hexdigest() == spec["sha256"], "artifact hash mismatch")
                return json.loads(data)
            result, code = evaluate(manifest, read, args.stage)
        except (Missing, ValueError, OSError) as error:
            result, code = {"status": "MISSING", "reason": str(error)}, 2
        print(json.dumps(result)); sys.exit(code)
