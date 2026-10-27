#!/usr/bin/env python3
"""Decode existing SVM coarse logs or pinned FEX v2 stats; do not measure or judge."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


SVM_TIMES = (
    "translate_ns", "decode_ns", "rpo_ns", "opt_ns", "regalloc_ns",
    "codegen_ns", "publish_ns", "ir_free_ns",
)
SVM_COUNTS = (
    "func_units", "block_units", "decoded_blocks", "host_bytes", "ir_insts", "pool_bytes",
)
SVM_ROWS = (set(SVM_TIMES), set(SVM_COUNTS), {"ir_arena_bytes"}, {"translate_wait_ns"})
HEADER = struct.Struct("<BB2s48sIII")  # pinned v2: 64 bytes, legacy padding at offset 2
THREAD = struct.Struct("<IIQQQQQ")  # 48 bytes, including five independent u64 counters
FEX_MAX_SIZE = 4 * 1024 * 1024
EVENTS = ("sigbus_count", "smc_count", "float_fallback_count")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def positive_integer(value):
    try:
        result = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a positive integer") from error
    if not 0 < result < 2**64:
        raise argparse.ArgumentTypeError("expected an integer in 1..2^64-1")
    return result


def svm_stats(data):
    values = {}
    prof2_lines = 0
    for number, line in enumerate(data.decode("utf-8").splitlines(), 1):
        if line.startswith("[svm-prof2]"):
            prof2_lines += 1
        if not line.startswith("[svm-prof]"):
            continue
        row = {}
        for token in line[len("[svm-prof]"):].split():
            match = re.fullmatch(r"([a-z_]+)=([0-9]+)", token)
            require(match is not None, f"line {number}: malformed coarse field {token!r}")
            key, value = match.groups()
            require(key not in row and key not in values,
                    f"line {number}: duplicate {key}; split multiple process dumps first")
            row[key] = int(value)
            require(row[key] < 2**64, f"line {number}: {key} exceeds its u64 counter")
        require(set(row) in SVM_ROWS, f"line {number}: unsupported coarse summary fields")
        values.update(row)
    expected = set().union(*SVM_ROWS)
    require(set(values) == expected, f"missing coarse fields: {', '.join(sorted(expected - values.keys()))}")
    return {
        "kind": "svm-coarse",
        "timing_ns": {key: values[key] for key in (*SVM_TIMES, "translate_wait_ns")},
        "counters": {key: values[key] for key in (*SVM_COUNTS, "ir_arena_bytes")},
        "prof2_lines_ignored": prof2_lines,
        "scope": {
            "translate_ns": "Whole x86 Impl::Translate after producer admission: config/range waits, "
                            "lookup and cache-hit returns, decode/retries, compilation, publication and teardown.",
            "translate_wait_ns": "Subset covering only translate_mutex and compile_ranges lock intervals; "
                                 "other internal waits remain in translate_ns.",
            "phases": "Existing coarse inclusive source buckets, preserved separately; no phase sum "
                      "or unaccounted-time estimate is calculated.",
            "units": "func_units/block_units/decoded_blocks are frontend requests, including retries; "
                     "they are not physical RA entry counts or proven cache misses.",
            "diagnostics": "PROF2 records are ignored, never summed. Their presence means the log "
                           "cannot establish a PROF=1-only run; environment and identity are external evidence.",
        },
    }


def fex_stats(data, clock_hz, clock_source):
    require(0 < clock_hz < 2**64, "clock_hz must be a positive u64 frequency")
    require(clock_source.strip(), "--clock-source must identify the frequency evidence")
    require(len(data) >= HEADER.size, "truncated FEX header")
    version, app_type, padding, version_text, head, size, pad = HEADER.unpack_from(data)
    require(version == 2, f"unsupported FEX stats version {version}; expected pinned v2")
    require(app_type in range(4), f"unknown FEX app_type {app_type}")
    require(padding == b"\0\0" and pad == 0, "unsupported nonzero pinned-v2 header padding")
    require(HEADER.size + 2 * THREAD.size <= size <= FEX_MAX_SIZE, "invalid FEX Size")
    require(size == len(data), f"FEX Size {size} differs from archived file length {len(data)}")
    stored_slots, trailing = divmod(size - HEADER.size, THREAD.size)
    # StatAllocBase::TotalSlotsFromSize deliberately leaves one complete slot unused.
    capacity = stored_slots - 1
    require(not any(data[HEADER.size + stored_slots * THREAD.size:]), "nonzero trailing slot padding")

    def check_offset(offset):
        require(offset == 0 or (
            offset >= HEADER.size
            and (offset - HEADER.size) % THREAD.size == 0
            and (offset - HEADER.size) // THREAD.size < capacity
        ), f"invalid Head/Next offset {offset}")

    check_offset(head)
    slots = []
    for index in range(stored_slots):
        offset = HEADER.size + index * THREAD.size
        fields = THREAD.unpack_from(data, offset)
        next_offset, tid, jit, signal, *events = fields
        check_offset(next_offset)
        require(index < capacity or not any(fields), "nonzero reserved final slot")
        slots.append({
            "index": index, "offset": offset, "next_offset": next_offset, "tid": tid,
            "allocatable": index < capacity, "empty": not any(fields),
            "jit_ticks": jit, "jit_ns_floor": jit * 1_000_000_000 // clock_hz,
            "signal_ticks": signal, "signal_ns_floor": signal * 1_000_000_000 // clock_hz,
            **dict(zip(EVENTS, events)),
        })
    linked = set()
    offset = head
    while offset:
        require(offset not in linked, f"cycle in FEX Head/Next chain at {offset}")
        linked.add(offset)
        offset = slots[(offset - HEADER.size) // THREAD.size]["next_offset"]
    active, residual = [], []
    for slot in slots:
        slot["linked_from_head"] = slot["offset"] in linked
        (active if slot["tid"] else residual).append(slot)
    tids = [slot["tid"] for slot in active]
    require(len(tids) == len(set(tids)), "duplicate active FEX TID")
    return {
        "kind": "fex-profile-stats-v2",
        "clock": {"hz": clock_hz, "source": clock_source,
                  "unit": "CNTVCT_EL0 ticks; not PMU cpu-cycles or CPU operating frequency",
                  "conversion": "ns_floor = ticks * 1000000000 // hz"},
        "header": {"version": version, "app_type": app_type,
                   "app_type_name": ("LINUX_32", "LINUX_64", "WIN_ARM64EC", "WIN_WOW64")[app_type],
                   "fex_version": version_text.split(b"\0", 1)[0].decode("utf-8", errors="backslashreplace"),
                   "fex_version_raw_hex": version_text.hex(), "head_offset": head, "size_bytes": size},
        "layout": {"byte_order": "little", "header_bytes": HEADER.size, "thread_bytes": THREAD.size,
                   "stored_slots": stored_slots, "allocatable_slots": capacity,
                   "trailing_zero_bytes": trailing},
        "active": active,
        "residual": residual,
        "snapshot": {"active_slots": len(active), "residual_slots": len(residual),
                     "nonempty_residual_slots": sum(not slot["empty"] for slot in residual),
                     "unlinked_active_offsets": [slot["offset"] for slot in active if not slot["linked_from_head"]],
                     "linked_inactive_offsets": [slot["offset"] for slot in residual if slot["linked_from_head"]]},
        "scope": {
            "jit": "CompileBlock inclusive elapsed ticks: PreCompile, invalidation lock, L3 cache-hit "
                   "returns, decode/build/passes/RA, emission/relocation, IR release and cache/SMC publication. "
                   "Excludes earlier thread/compiler initialization and dispatcher L1/L2 lookup.",
            "signal": "Independent AccumulatedSignalTime; never added to JIT by this decoder.",
            "residual": "All TID=0 slots, including empty/reserved slots. Counters can survive unlink but "
                        "are reset on slot reuse. No process or all-thread lifetime total is inferred.",
            "links": "Every nonzero Head/Next is range/alignment checked; the Head chain is cycle checked. "
                     "Unlinked residual Next values are stale links, not a second active chain.",
            "provenance": "Caller must supply PID/starttime, live-FD capture, thread/reuse history, output/exit "
                          "and ELF/config evidence separately. A decoded file is not a comparison verdict.",
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="kind", required=True)
    svm = commands.add_parser("svm", help="decode one existing SVM coarse stderr summary")
    fex = commands.add_parser("fex", help="decode a complete pinned FEX v2 stats file")
    for command in (svm, fex):
        command.add_argument("input", type=Path)
        command.add_argument("--output", type=Path, help="write JSON here; default is stdout")
    fex.add_argument("--clock-hz", type=positive_integer, required=True,
                     help="explicit CNTFRQ_EL0 frequency, not CPU MHz or PMU cycles")
    fex.add_argument("--clock-source", required=True,
                     help="frequency provenance, e.g. path:line and SHA-256 of the existing metrics output")
    args = parser.parse_args()
    try:
        require(args.output is None or args.output.resolve() != args.input.resolve(),
                "output must not overwrite the raw input")
        if args.output and args.output.exists():
            require(not args.output.samefile(args.input), "output is a hard link to the raw input")
        data = args.input.read_bytes()
        result = svm_stats(data) if args.kind == "svm" else fex_stats(data, args.clock_hz, args.clock_source)
        result = {"schema": 1, "source": {"path": str(args.input.resolve()), "bytes": len(data),
                                         "sha256": hashlib.sha256(data).hexdigest()}, **result}
        text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
    except (OSError, ValueError, struct.error) as error:
        print(f"compile_stats: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
