#!/usr/bin/env python3
"""Build and audit the INT_IMM_FOLD placement 2x2 calibration artifacts."""

import argparse
import re
from pathlib import Path


REFERENCE_RE = re.compile(
    r"\[svm-placement-reference\] kind=(\w+) unit=(0x[0-9a-f]+) "
    r"pc=(0x[0-9a-f]+) offset=(\d+)"
)
HOST_RE = re.compile(
    r"\[svm-host\] pc=(0x[0-9a-f]+) size=(\d+) hash=([0-9a-f]+) "
    r"bytes=([0-9a-f]+)"
)
PAD_RE = re.compile(
    r"\[svm-placement-pad\] kind=(\w+) unit=(0x[0-9a-f]+) "
    r"pc=(0x[0-9a-f]+) before=(\d+) after=(\d+) ops=(\d+)"
)


def command_reference(args: argparse.Namespace) -> None:
    text = args.log.read_text(errors="replace")
    rows = REFERENCE_RE.findall(text)
    if not rows:
        raise SystemExit("no placement reference records found")
    unique = {}
    for kind, unit, pc, offset in rows:
        key = (kind, int(unit, 16), int(pc, 16))
        value = int(offset)
        previous = unique.setdefault(key, value)
        if previous != value:
            raise SystemExit(
                f"non-deterministic reference {key}: {previous} vs {value}"
            )
    lines = [
        f"{kind} {unit:#x} {pc:#x} {offset}\n"
        for (kind, unit, pc), offset in sorted(unique.items())
    ]
    args.output.write_text("".join(lines))
    print(f"reference_points={len(unique)} output={args.output}")


def hosts(path: Path):
    units = {}
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if "[svm-host]" not in line:
            continue
        match = HOST_RE.fullmatch(line.strip())
        if not match:
            raise SystemExit(f"{path}:{line_number}: incomplete host record")
        pc, size, digest, bytes_ = match.groups()
        address, size = int(pc, 16), int(size)
        if size == 0 or len(bytes_) != size * 2:
            raise SystemExit(f"{path}:{line_number}: host byte count differs from size")
        checksum = 1469598103934665603
        for byte in bytes.fromhex(bytes_):
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        if checksum != int(digest, 16):
            raise SystemExit(f"{path}:{line_number}: host checksum differs from bytes")
        record = (size, digest, bytes_)
        previous = units.setdefault(address, record)
        if previous != record:
            raise SystemExit(f"{path}:{line_number}: conflicting host records for {address:#x}")
    if not units:
        raise SystemExit(f"{path}: no host records found")
    return units


def command_audit(args: argparse.Namespace) -> None:
    arms = {name: hosts(path) for name, path in (
        ("cell1", args.cell1), ("cell2", args.cell2),
        ("cell3", args.cell3), ("cell4", args.cell4),
    )}
    for name, units in arms.items():
        print(f"{name}: units={len(units)} bytes={sum(v[0] for v in units.values())}")
    expected = set(arms["cell1"])
    for name, units in arms.items():
        missing, extra = expected - set(units), set(units) - expected
        if missing or extra:
            raise SystemExit(f"{name}: unit addresses differ: "
                             f"missing={sorted(missing)} extra={sorted(extra)}")
    for left, right in (("cell1", "cell2"), ("cell2", "cell3"),
                        ("cell2", "cell4"), ("cell3", "cell4")):
        a, b = arms[left], arms[right]
        common = sorted(set(a) & set(b))
        size_diff = [pc for pc in common if a[pc][0] != b[pc][0]]
        byte_diff = [pc for pc in common if a[pc][2] != b[pc][2]]
        print(
            f"{left}/{right}: common={len(common)} "
            f"size_diff={len(size_diff)} "
            f"size_delta={sum(b[pc][0] - a[pc][0] for pc in size_diff)} "
            f"byte_diff={len(byte_diff)}"
        )
    pads = PAD_RE.findall(args.cell2.read_text(errors="replace"))
    print(f"cell2_pad_points={len(pads)} pad_ops={sum(int(row[5]) for row in pads)}")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    ref = sub.add_parser("reference")
    ref.add_argument("log", type=Path)
    ref.add_argument("output", type=Path)
    ref.set_defaults(func=command_reference)
    audit = sub.add_parser("audit")
    audit.add_argument("cell1", type=Path)
    audit.add_argument("cell2", type=Path)
    audit.add_argument("cell3", type=Path)
    audit.add_argument("cell4", type=Path)
    audit.set_defaults(func=command_audit)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
