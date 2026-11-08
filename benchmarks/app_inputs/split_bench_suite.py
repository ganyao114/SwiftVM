#!/usr/bin/env python3
"""Give the frozen bench-suite ELF RX code and RW BSS without changing its code."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


INPUT_SHA256 = "df60aa3f57d40ed3eb0b44d124f2cb8c9d15899f90021b6d20e94be179240a6d"
BASE = 0x400000
BSS = 0x410000  # The input's sole mutable global: 64-KiB-aligned bench_mem_arr.
PAGE = 4096
PHDR = struct.Struct("<IIQQQQQQ")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    original = args.guest.read_bytes()
    if digest(original) != INPUT_SHA256:
        parser.error("this tool accepts only the frozen 6272-byte bench-suite input")
    if args.output.resolve() == args.guest.resolve():
        parser.error("output must be separate from the frozen input")

    phoff = struct.unpack_from("<Q", original, 32)[0]
    old = PHDR.unpack_from(original, phoff)
    assert old == (1, 7, 0, BASE, BASE, len(original), 4259840, PAGE)
    entry = struct.unpack_from("<Q", original, 24)[0]
    # A second header would overlap _start in the original compact ELF.
    # Append the new table inside RX instead; all original instruction and
    # constant bytes, guest PCs and data addresses retain their original layout.
    table_offset = (len(original) + 7) & ~7
    table_end = table_offset + 2 * PHDR.size
    data_offset = (table_end + PAGE - 1) & -PAGE
    output = bytearray(original)
    output.extend(bytes(data_offset - len(output)))
    struct.pack_into("<Q", output, 32, table_offset)
    struct.pack_into("<H", output, 56, 2)
    rx = (1, 5, 0, BASE, BASE, table_end, BSS - BASE, PAGE)
    rw = (1, 6, data_offset, BSS, BSS, 0, old[6] - (BSS - BASE), PAGE)
    PHDR.pack_into(output, table_offset, *rx)
    PHDR.pack_into(output, table_offset + PHDR.size, *rw)
    assert output[entry - BASE:len(original)] == original[entry - BASE:]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    args.output.chmod(0o755)
    manifest = {
        "input": {"path": str(args.guest), "sha256": digest(original)},
        "output": {"path": str(args.output), "bytes": len(output), "sha256": digest(output)},
        "entry_pc": entry,
        "code_and_constants_sha256": digest(original[entry - BASE:]),
        "instruction_and_constant_bytes_unchanged": True,
        "guest_pcs_and_bss_addresses_unchanged": True,
        "segments": {"rx": rx, "rw": rw},
        "comparison": "Separate formation diagnostic; never replaces the original RWX baseline.",
    }
    args.output.with_suffix(args.output.suffix + ".json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
