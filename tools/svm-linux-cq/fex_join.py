#!/usr/bin/env python3
"""Join SwiftVM hot units against FEX blockstats by guest-range containment.

Inputs:
  --svm   a `SVM_RA_HOT_COALESCE` log carrying `[svm-hot-all] pc entries host_static`
  --fex   a FEX stderr capture with `[fex-blockstat] rip guest_bytes guest_inst host_inst`

This is a code-range structure estimate, not a dynamic-work or speed comparison.
The input does not carry matched FEX execution counts. Grouping SwiftVM roots
can also include guest instructions that a particular entry does not execute.

Join rule: every SwiftVM unit pc falls into the tightest FEX block whose
[rip, rip+guest_bytes) span contains it. Report static code volume separately
from SwiftVM's per-code entry-weighted static count. Neither metric proves
equal guest work, and no FEX execution count is inferred from SwiftVM entries.
"""

from __future__ import annotations

import argparse
import bisect
import re
import sys

from hot_records import load_hot, read_fields


GAP_LINE = re.compile(
    r"\[svm-gap-op\]\s+(?:unit=(0x[0-9a-f]+)\s+)?block=(0x[0-9a-f]+)\s+guest_pc=(0x[0-9a-f]+)"
)


def parse_gap_members(path: str) -> dict[int, set[int]]:
    """Per unit root pc -> set of member guest PCs (from SVM_DENSITY_PROF).

    Newer logs carry `unit=` naming the owning code object's root; older logs
    only name the IR block, so `block=` is the fallback key.
    """
    members: dict[int, set[int]] = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if "[svm-gap-op]" not in line:
                continue
            m = GAP_LINE.search(line)
            if not m:
                raise ValueError("incomplete guest membership record")
            key = int(m.group(1) or m.group(2), 16)
            members.setdefault(key, set()).add(int(m.group(3), 16))
    if not members:
        raise ValueError("no guest membership records")
    return members


def parse_fex(path: str) -> list[tuple[int, int, int, int]]:
    blocks: dict[int, tuple[int, int, int]] = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if "[fex-blockstat]" not in line:
                continue
            fields = read_fields(line, "[fex-blockstat]",
                                 ('rip', 'guest_bytes', 'guest_inst', 'host_inst'))
            rip = fields['rip']
            shape = tuple(fields[name] for name in ('guest_bytes', 'guest_inst', 'host_inst'))
            if not all(shape) or rip + shape[0] > 1 << 64:
                raise ValueError(f"invalid FEX code extent at {rip:#x}")
            if rip in blocks and blocks[rip] != shape:
                raise ValueError(f"conflicting FEX code versions at {rip:#x}")
            blocks[rip] = shape
    if not blocks:
        raise ValueError("no FEX block records")
    return [(rip, *vals) for rip, vals in sorted(blocks.items())]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--svm", required=True)
    ap.add_argument("--fex", required=True)
    ap.add_argument("--gap", help="SVM_DENSITY_PROF stderr for member guest PCs")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--min-coverage", type=float, default=99.9)
    args = ap.parse_args()

    if args.top < 0 or not 0 <= args.min_coverage <= 100:
        raise ValueError("invalid --top or --min-coverage")
    svm = load_hot(args.svm)
    members = parse_gap_members(args.gap) if args.gap else {}
    fex = parse_fex(args.fex)

    starts = [b[0] for b in fex]

    # block -> [static code volume, entries, unit count, per-code weighted count]
    per_block: dict[int, list[int]] = {}
    uncovered_entries = 0
    uncovered_host = 0
    total_entries = sum(unit.entries for unit in svm.values())
    total_svm_host = sum(unit.weighted_host() for unit in svm.values())
    if not total_entries or not total_svm_host:
        raise ValueError("no executed SwiftVM code in capture")

    max_span = max(b[1] for b in fex)

    for pc, unit in svm.items():
        # candidate blocks: any [rip, rip+bytes) containing pc -> find tightest
        best = -1
        best_span = 1 << 62
        j = bisect.bisect_right(starts, pc) - 1
        while j >= 0:
            rip, gbytes, ginst, hinst = fex[j]
            if rip < pc - max_span:
                break  # starts decrease; no earlier block can reach pc
            contains_members = not args.gap or (
                pc in members and all(rip <= member < rip + gbytes for member in members[pc]))
            if gbytes > pc - rip and gbytes < best_span and contains_members:
                best_span = gbytes
                best = j
            j -= 1
        if best < 0:
            uncovered_entries += unit.entries
            uncovered_host += unit.weighted_host()
            continue
        rec = per_block.setdefault(best, [0, 0, 0, 0])
        rec[0] += unit.host_static
        rec[1] += unit.entries
        rec[2] += 1
        rec[3] += unit.weighted_host()

    covered_entries = total_entries - uncovered_entries
    covered_svm_host = total_svm_host - uncovered_host

    # static (unweighted) comparison over the same matched set
    static_svm = sum(r[0] for r in per_block.values())
    static_fex = sum(fex[i][3] for i in per_block)
    static_fex_guest = sum(fex[i][2] for i in per_block)

    rows = []
    for idx, (svm_host_sum, entries, npcs, weighted) in per_block.items():
        rip, gbytes, ginst, hinst = fex[idx]
        rows.append((svm_host_sum - hinst, rip, svm_host_sum, hinst,
                     entries, npcs, weighted))

    entry_coverage = 100.0 * covered_entries / total_entries
    host_coverage = 100.0 * covered_svm_host / total_svm_host
    print(f"svm_pcs={len(svm)} fex_blocks={len(fex)} matched_blocks={len(per_block)}")
    print(f"entry_coverage={entry_coverage:.6f}% svm_host_coverage={host_coverage:.6f}%")
    print("comparison_kind=range_volume_estimate; not dynamic-work or speed ratios")
    print(f"entry_weighted_static: svm={covered_svm_host} total={total_svm_host} "
          f"uncovered={uncovered_host}; not executed instruction counts")
    print(f"static: svm_host={static_svm} fex_host={static_fex} "
          f"svm/fex={static_svm / max(1, static_fex):.6f} "
          f"(guest_inst={static_fex_guest})")
    rows.sort(key=lambda r: -r[0])
    print("\nLargest static range differences:")
    for delta, rip, sh, fh, entries, npcs, weighted in rows[: args.top]:
        print(f"  0x{rip:x} svm={sh} fex={fh} delta={delta:+d} "
              f"entries={entries} svm_entry_weighted={weighted} pcs={npcs}")
    ok = bool(per_block) and min(entry_coverage, host_coverage) >= args.min_coverage
    print(f"coverage_status={'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(2) from exc
