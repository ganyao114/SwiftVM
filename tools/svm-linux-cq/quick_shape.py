#!/usr/bin/env python3
from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import os
import pathlib
import platform
import re
import sys

from hot_records import load_hot
from run_limits import RunLimits, kernel_limits, run_limited


PROFILE_ENV = (
    "SVM_JIT_CACHE",
    "SVM_RA_SHAPE_PROF",
    "SVM_RA_DIAG",
    "SVM_EXEC_PROF",
    "SVM_EXEC_TRACE",
    "SVM_DENSITY_PROF",
    "SVM_INDIRECT_L1_PROF",
    "SVM_DECODE_PROF",
    "SVM_SIGNAL_TRACE",
    "SVM_MEM_MODE_TRACE",
    "SVM_PROF",
    "SVM_PROF2",
    "SVM_RA_HOT_COALESCE",
    "SVM_RA_HOT_COALESCE_ALL",
    "SVM_VIXL_HOST_DUMP",
)

HOST_DUMP_LINE = re.compile(
    rb"^\[svm-host\]\s+pc=(0x[0-9a-f]+)\s+size=(\d+)\s+",
    re.MULTILINE,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture candidate code shape without detailed density logging."
    )
    parser.add_argument("--svm", required=True, type=pathlib.Path)
    parser.add_argument("--guest", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--cpu-percent", type=float, default=10.0)
    parser.add_argument("--cpu-seconds", type=float, default=4.0)
    parser.add_argument("--rss-mib", type=int, default=192)
    parser.add_argument("--expect-exit", type=int, default=0)
    parser.add_argument("--input", action="append", type=pathlib.Path, default=[])
    parser.add_argument("--oracle", action="append", default=[])
    parser.add_argument("--expect-output", action="append", default=[], metavar="FILE=SHA256")
    parser.add_argument(
        "--static-only",
        action="store_true",
        help="capture host shape without emitting runtime entry counters",
    )
    parser.add_argument("guest_args", nargs=argparse.REMAINDER)
    return parser.parse_args()


def ensure_output_directory(path: pathlib.Path) -> None:
    if path.exists() and any(path.iterdir()):
        raise ValueError(f"output directory is not empty: {path}")
    path.mkdir(parents=True, exist_ok=True)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_details(path: pathlib.Path) -> dict:
    return {"path": str(path), "sha256": sha256(path), "bytes": path.stat().st_size}


def output_file(output: pathlib.Path, name: str) -> pathlib.Path:
    path = output / name
    if pathlib.Path(name).is_absolute() or not path.resolve().is_relative_to(output):
        raise ValueError(f"result must be inside the output directory: {name}")
    return path


def write_static_shape(stderr_path: pathlib.Path, hot_path: pathlib.Path) -> int:
    versions: dict[int, list[int]] = {}
    for match in HOST_DUMP_LINE.finditer(stderr_path.read_bytes()):
        pc = int(match.group(1), 0)
        versions.setdefault(pc, []).append(int(match.group(2)))
    with hot_path.open("w", encoding="utf-8") as handle:
        number = 0
        for pc, sizes in sorted(versions.items()):
            for size in sizes:
                if not size or size % 4:
                    raise ValueError(f"invalid host code size for 0x{pc:x}: {size}")
                handle.write(
                    f"[svm-hot-code] pc=0x{pc:x} code={number} entries=0 "
                    f"host_bytes={size} host_static={size // 4}\n"
                )
                number += 1
            host_bytes = max(sizes)
            handle.write(
                f"[svm-hot-all] pc=0x{pc:x} versions={len(sizes)} entries=0 "
                f"host_bytes={host_bytes} host_static={host_bytes // 4} "
                "move_static=0 nan_static=0 spill_static=0 state_saved_static=0\n"
            )
        handle.write(f"[svm-hot-end] codes={number} pcs={len(versions)} overflow=0\n")
    return len(versions)


def main() -> int:
    args = parse_args()
    svm = args.svm.resolve()
    guest = args.guest.resolve()
    output = args.out.resolve()
    limits = RunLimits(wall_seconds=args.timeout, cpu_fraction=args.cpu_percent / 100,
                       cpu_seconds=args.cpu_seconds, rss_mib=args.rss_mib)
    limits.validate()
    if not svm.is_file():
        raise ValueError(f"SVM executable does not exist: {svm}")
    if not guest.is_file():
        raise ValueError(f"guest executable does not exist: {guest}")
    expected_outputs = {}
    for item in args.expect_output:
        name, separator, digest = item.rpartition("=")
        if not separator or not name or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ValueError("--expect-output requires FILE=SHA256")
        if name in expected_outputs and expected_outputs[name] != digest.lower():
            raise ValueError(f"conflicting expected output for {name}")
        expected_outputs[name] = digest.lower()
    result_names = set(args.oracle) | set(expected_outputs)
    for name in result_names:
        output_file(output, name)
    inputs = [file_details(path.resolve()) for path in [svm, guest, *args.input]]
    ensure_output_directory(output)

    hot_path = output / "shape.hot"
    stdout_path = output / "stdout.log"
    stderr_path = output / "stderr.log"
    hot_path.unlink(missing_ok=True)
    environment = os.environ.copy()
    for name in PROFILE_ENV:
        environment.pop(name, None)
    environment["SVM_JIT_CACHE"] = ""
    if args.static_only:
        environment["SVM_VIXL_HOST_DUMP"] = "1"
    else:
        environment["SVM_RA_HOT_COALESCE"] = str(hot_path)
        environment["SVM_RA_HOT_COALESCE_ALL"] = "1"

    guest_args = args.guest_args[1:] if args.guest_args[:1] == ["--"] else args.guest_args
    command = [str(svm), str(guest), *guest_args]
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        completed = run_limited(command, cwd=output, env=environment, stdout=stdout,
                                stderr=stderr, limits=limits)

    errors = []
    if completed.stop_reason:
        errors.append(completed.stop_reason)
    if completed.return_code != args.expect_exit:
        errors.append(f"expected exit {args.expect_exit}, got {completed.return_code}")
    for details in inputs:
        path = pathlib.Path(details["path"])
        if not path.is_file() or sha256(path) != details["sha256"]:
            errors.append(f"input changed during execution: {path}")

    hot_records = 0
    try:
        if args.static_only:
            write_static_shape(stderr_path, hot_path)
        units = load_hot(hot_path)
        hot_records = len(units)
        if not args.static_only and not any(unit.entries for unit in units.values()):
            errors.append("capture contains no executed units")
    except (OSError, ValueError) as exc:
        errors.append(str(exc))

    outputs = {}
    for name in sorted(result_names | {"stdout.log", "stderr.log", "shape.hot"}):
        path = output_file(output, name)
        if not path.is_file():
            errors.append(f"result missing: {name}")
            continue
        outputs[name] = file_details(path)
        if name in expected_outputs and outputs[name]["sha256"] != expected_outputs[name]:
            errors.append(f"result checksum differs: {name}")

    record = {
        "format_version": 1,
        "mode": "static_bytes" if args.static_only else "entry_weighted_static",
        "host": {"system": platform.system(), "machine": platform.machine()},
        "command": command, "cwd": str(output), "inputs": inputs,
        "environment": {k: v for k, v in environment.items() if k.startswith("SVM_")},
        "limits": asdict(limits), "result": asdict(completed),
        "kernel_limits": kernel_limits(),
        "elapsed_is_performance_measurement": False,
        "expected_exit": args.expect_exit, "expected_outputs": expected_outputs,
        "outputs": outputs, "hot_records": hot_records,
        "valid": not errors, "errors": errors,
    }
    (output / "run.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")

    print(
        f"rc={completed.return_code} elapsed={completed.elapsed_s:.3f}s hot_records={hot_records} "
        f"stderr_bytes={stderr_path.stat().st_size}"
    )
    print(f"capture_valid={not errors}; elapsed includes CPU throttling")
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(2) from exc
