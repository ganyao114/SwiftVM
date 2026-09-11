#!/usr/bin/env python3
"""Join SwiftVM hot units against FEX blockstats by guest-range containment.

Inputs:
  --svm   a `SVM_RA_HOT_COALESCE` log carrying `[svm-hot-all] pc entries host_static`
  --fex   a FEX stderr capture with `[fex-blockstat] rip guest_bytes guest_inst host_inst`

Join rule: every SwiftVM unit pc falls into the tightest FEX block whose
[rip, rip+guest_bytes) span contains it. Per FEX block we compare the block's
host_inst against the sum of the covered SwiftVM units' host_static, weighted by
the covered SwiftVM entries. This avoids needing per-unit guest_inst: the guest
range is identical for both sides of a block.
"""

from __future__ import annotations

import argparse
import bisect
import re
import sys

SVM_LINE = re.compile(
    r"\[svm-hot-all\]\s+pc=(0x[0-9a-f]+)\s+versions=(\d+)\s+entries=(\d+)\s+"
    r"host_bytes=(\d+)\s+host_static=(\d+)"
)
FEX_LINE = re.compile(
    r"\[fex-blockstat\]\s+rip=(0x[0-9a-f]+)\s+guest_bytes=(\d+)\s+"
    r"guest_inst=(\d+)\s+host_inst=(\d+)"
)


def parse_svm(path: str) -> dict[int, tuple[int, int]]:
    units: dict[int, list[int]] = {}
    for line in open(path, errors="replace"):
        m = SVM_LINE.search(line)
        if not m:
            continue
        pc = int(m.group(1), 16)
        entries = int(m.group(3))
        host = int(m.group(5))
        units.setdefault(pc, []).append((entries, host))
    # same-pc duplicates: keep max host_static, sum entries
    return {pc: (sum(e for e, _ in v), max(h for _, h in v)) for pc, v in units.items()}


GAP_LINE = re.compile(
    r"\[svm-gap-op\]\s+(?:unit=(0x[0-9a-f]+)\s+)?block=(0x[0-9a-f]+)\s+guest_pc=(0x[0-9a-f]+)"
)
GAP_BLOCK = re.compile(
    r"\[svm-gap-block\]\s+unit=(0x[0-9a-f]+)\s+block=(0x[0-9a-f]+)\s+"
    r"bytes=(\d+)\s+insts=(\d+)"
)


def parse_gap_members(path: str) -> dict[int, set[int]]:
    """Per unit root pc -> set of member guest PCs (from SVM_DENSITY_PROF).

    Newer logs carry `unit=` naming the owning code object's root; older logs
    only name the IR block, so `block=` is the fallback key.
    """
    members: dict[int, set[int]] = {}
    try:
        handle = open(path, "rb")
    except OSError:
        return members
    for raw in handle:
        m = GAP_LINE.search(raw.decode("utf-8", "replace"))
        if not m:
            continue
        key = int(m.group(1) or m.group(2), 16)
        members.setdefault(key, set()).add(int(m.group(3), 16))
    return members


def parse_gap_blocks(path: str) -> dict[int, int]:
    """Per IR-block pc -> decoded guest instruction count (authoritative)."""
    insts: dict[int, int] = {}
    try:
        handle = open(path, "rb")
    except OSError:
        return insts
    for raw in handle:
        m = GAP_BLOCK.search(raw.decode("utf-8", "replace"))
        if m:
            insts[int(m.group(2), 16)] = int(m.group(4))
    return insts


def parse_fex(path: str) -> list[tuple[int, int, int, int]]:
    blocks: dict[int, tuple[int, int, int]] = {}
    for line in open(path, errors="replace"):
        m = FEX_LINE.search(line)
        if not m:
            continue
        rip = int(m.group(1), 16)
        blocks[rip] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    out = [(rip, *vals) for rip, vals in sorted(blocks.items())]
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--svm", required=True)
    ap.add_argument("--fex", required=True)
    ap.add_argument("--gap", help="SVM_DENSITY_PROF stderr for member guest PCs")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    svm = parse_svm(args.svm)
    members = parse_gap_members(args.gap) if args.gap else {}
    block_insts = parse_gap_blocks(args.gap) if args.gap else {}
    fex = parse_fex(args.fex)
    if not svm or not fex:
        print("missing input data", file=sys.stderr)
        return 2

    starts = [b[0] for b in fex]
    ends = [b[0] + b[1] for b in fex]

    # block -> [svm_host_sum, covered_entries, covered_pcs]
    per_block: dict[int, list[int]] = {}
    uncovered_entries = 0
    uncovered_host = 0
    total_entries = sum(e for e, _ in svm.values())
    total_svm_host = sum(e * h for e, h in svm.values())

    max_span = max(b[1] for b in fex)

    # doc-formula per-PC join: each svm pc carries guest_inst = its member
    # count (when --gap is given); the containing FEX block contributes
    # host_inst/guest_inst weighted by the pc's entries.
    pc_num = 0  # sum_p e_p * svm_host(p)
    pc_den_g = 0  # sum_p e_p * svm_guest_inst(p)
    pc_fex_num = 0  # sum_p e_p * fex host_inst(B(p))
    pc_fex_den = 0  # sum_p e_p * fex guest_inst(B(p))
    pc_gap_missing = 0

    for pc, (entries, host) in svm.items():
        # candidate blocks: any [rip, rip+bytes) containing pc -> find tightest
        best = -1
        best_span = 1 << 62
        j = bisect.bisect_right(starts, pc) - 1
        while j >= 0:
            rip, gbytes, ginst, hinst = fex[j]
            if rip < pc - max_span:
                break  # starts decrease; no earlier block can reach pc
            if gbytes > pc - rip and gbytes < best_span:
                best_span = gbytes
                best = j
            j -= 1
        if best < 0:
            uncovered_entries += entries
            uncovered_host += entries * host
            continue
        rip, gbytes, ginst, hinst = fex[best]
        rec = per_block.setdefault(best, [0, 0, 0])
        rec[0] += host
        rec[1] += entries
        rec[2] += 1
        if members or block_insts:
            g = block_insts.get(pc, 0) or len(members.get(pc, ()))
            if g == 0:
                pc_gap_missing += 1
                continue
            pc_num += entries * host
            pc_den_g += entries * g
            pc_fex_num += entries * hinst
            pc_fex_den += entries * ginst

    covered_entries = total_entries - uncovered_entries
    covered_svm_host = total_svm_host - uncovered_host

    # static (unweighted) comparison over the same matched set
    static_svm = sum(r[0] for r in per_block.values())
    static_fex = sum(fex[i][3] for i in per_block)
    static_fex_guest = sum(fex[i][2] for i in per_block)

    num = 0  # sum_b e_b * svm_host_b
    den = 0  # sum_b e_b * fex_host_b
    fex_guest_w = 0
    svm_guest_w = 0
    rows = []
    for idx, (svm_host_sum, entries, npcs) in per_block.items():
        rip, gbytes, ginst, hinst = fex[idx]
        num += entries * svm_host_sum
        den += entries * hinst
        fex_guest_w += entries * ginst
        rows.append((entries * (svm_host_sum - hinst), rip, svm_host_sum, hinst,
                     entries, npcs, ginst))

    ratio = num / den if den else float("nan")
    print(f"svm_pcs={len(svm)} fex_blocks={len(fex)} matched_blocks={len(per_block)}")
    print(f"entry_coverage={100.0 * covered_entries / total_entries:.6f}% "
          f"svm_host_coverage={100.0 * covered_svm_host / total_svm_host:.6f}%")
    print(f"weighted: svm_host={num} fex_host={den} svm/fex={ratio:.6f}")
    print(f"weighted guest_inst (fex-side reference)={fex_guest_w}")
    print(f"equiv blow-up svm={num / max(1, fex_guest_w):.4f} "
          f"fex={den / max(1, fex_guest_w):.4f} host/guest-inst")
    print(f"static: svm_host={static_svm} fex_host={static_fex} "
          f"svm/fex={static_svm / max(1, static_fex):.6f} "
          f"(guest_inst={static_fex_guest})")
    if members:
        print(
            f"per-pc join (gap members): svm={pc_num / max(1, pc_den_g):.6f} "
            f"fex={pc_fex_num / max(1, pc_fex_den):.6f} host/guest-inst, "
            f"ratio={pc_num / max(1, pc_den_g) / (pc_fex_num / max(1, pc_fex_den)):.6f} "
            f"no-member-pcs={pc_gap_missing}"
        )

    rows.sort(key=lambda r: -r[0])
    print(f"\ntop positive gaps (svm_host_sum - fex_host_inst, entries-weighted):")
    for delta_w, rip, sh, fh, e, npcs, ginst in rows[: args.top]:
        print(f"  0x{rip:x} svm={sh} fex={fh} +{sh - fh} "
              f"entries={e} weighted=+{delta_w} pcs={npcs} guest_inst={ginst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
