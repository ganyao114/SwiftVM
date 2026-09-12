#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import sys

from hot_records import HotUnit, load_hot as read_hot_records


def load_hot(path: pathlib.Path) -> dict[int, HotUnit]:
    result = read_hot_records(path)
    # Equal version counts do not establish corresponding code bodies across
    # separate builds. Retained weights may only select an unambiguous shape.
    if any(unit.versions != 1 for unit in result.values()):
        raise ValueError(f"cannot match multiple code versions across captures: {path}")
    return result


def percent(part: int, whole: int) -> float:
    return 100.0 * part / whole if whole else 0.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Apply retained formal entries to a short candidate code-shape run."
    )
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("candidate", type=pathlib.Path)
    parser.add_argument("--weights", type=pathlib.Path)
    parser.add_argument("--min-coverage", type=float, default=99.9)
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--limit", type=int, default=10)
    parser.add_argument("--fail-on-growth", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not 0.0 <= args.min_coverage <= 100.0:
        raise ValueError("--min-coverage must be between 0 and 100")
    if args.top < 0 or args.limit < 0:
        raise ValueError("--top and --limit must be non-negative")

    weights = load_hot(args.weights or args.baseline)
    baseline = load_hot(args.baseline)
    candidate = load_hot(args.candidate)
    exact = {
        pc
        for pc, shape in weights.items()
        if pc in baseline
        and pc in candidate
        and baseline[pc].versions == shape.versions
        and candidate[pc].versions == shape.versions
    }
    version_mismatches = {
        pc
        for pc in weights.keys() & baseline.keys() & candidate.keys()
        if weights[pc].versions != baseline[pc].versions
        or weights[pc].versions != candidate[pc].versions
    }
    baseline_missing = weights.keys() - baseline.keys()
    candidate_missing = weights.keys() - candidate.keys()
    extra = candidate.keys() - weights.keys()

    total_host = sum(shape.weighted_host() for shape in weights.values())
    total_entries = sum(shape.entries for shape in weights.values())
    if not total_host or not total_entries:
        raise ValueError("weights contain no executed code")
    covered_host = sum(weights[pc].weighted_host() for pc in exact)
    covered_entries = sum(weights[pc].entries for pc in exact)
    baseline_common = sum(
        weights[pc].entries * baseline[pc].host_static for pc in exact
    )
    candidate_common = sum(
        weights[pc].entries * candidate[pc].host_static for pc in exact
    )
    delta = candidate_common - baseline_common

    ranked = sorted(
        weights,
        key=lambda pc: (-weights[pc].weighted_host(), pc),
    )
    top = ranked[: args.top]
    top_matched = sum(pc in exact for pc in top)
    host_coverage = percent(covered_host, total_host)
    entry_coverage = percent(covered_entries, total_entries)

    print("comparison_kind=retained_entry_static_estimate; not dynamic-work or speed ratios")
    print(
        f"weights pcs={len(weights)} versions={sum(x.versions for x in weights.values())} "
        f"weighted_host={total_host}"
    )
    print(
        f"baseline pcs={len(baseline)} versions={sum(x.versions for x in baseline.values())}"
    )
    print(
        f"candidate pcs={len(candidate)} versions={sum(x.versions for x in candidate.values())}"
    )
    print(
        f"matched pcs={len(exact)} top={top_matched}/{len(top)} "
        f"host_coverage={host_coverage:.6f}% entry_coverage={entry_coverage:.6f}%"
    )
    print(
        f"baseline_missing={len(baseline_missing)} candidate_missing={len(candidate_missing)} "
        f"version_mismatch={len(version_mismatches)} extra={len(extra)} "
        f"uncovered_host={total_host - covered_host}"
    )
    print(
        f"common_baseline={baseline_common} common_candidate={candidate_common} "
        f"delta={delta:+d} delta_pct={percent(delta, baseline_common):+.6f}%"
    )
    uncovered_top = [pc for pc in top if pc not in exact]
    if uncovered_top:
        print("top_uncovered=" + ",".join(f"0x{pc:x}" for pc in uncovered_top))

    changes = []
    for pc in exact:
        before = baseline[pc].host_static
        after = candidate[pc].host_static
        if before != after:
            changes.append((weights[pc].entries * (after - before), pc, before, after))
    changes.sort(key=lambda item: (-abs(item[0]), item[1]))
    for weighted_delta, pc, before, after in changes[: args.limit]:
        print(
            f"pc=0x{pc:x} entries={weights[pc].entries} host={before}->{after} "
            f"delta={weighted_delta:+d}"
        )

    coverage_ok = (bool(exact) and min(host_coverage, entry_coverage) >= args.min_coverage
                   and top_matched == len(top))
    growth_ok = not args.fail_on_growth or delta <= 0
    print(f"status={'PASS' if coverage_ok and growth_ok else 'FAIL'}")
    return 0 if coverage_ok and growth_ok else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(2) from exc
