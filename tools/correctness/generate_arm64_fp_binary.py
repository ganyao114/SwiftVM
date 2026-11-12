#!/usr/bin/env python3
"""Generate compiler-encoded scalar/NEON/indexed ARM FP differential cases.

Uses the existing unmodified SVMCASE comparison contract and independent oracle.
Destination overlap, inactive lanes, sticky status and all supported controls
are tested alongside inputs immediately inside/outside the native fast range.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess

import oracle


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clang", default="clang-18")
    parser.add_argument("--objcopy", default="llvm-objcopy-18")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    forms = []
    for bits in (32, 64):
        scalar = "s" if bits == 32 else "d"
        shapes = ("2s", "4s") if bits == 32 else ("2d",)
        for operation in ("fadd", "fsub", "fmul"):
            forms.append((bits, 1, None, operation, scalar))
            for shape in shapes:
                forms.append((bits, int(shape[0]), None, operation, shape))
        for index in range(128 // bits):
            forms.append((bits, 1, index, "fmul", scalar))
            for shape in shapes:
                forms.append((bits, int(shape[0]), index, "fmul", shape))
    assemblies = []
    for bits, lanes, index, operation, shape in forms:
        for destination in (0, 1, 2):
            lhs = f"v1.{shape}" if shape[0].isdigit() else f"{shape}1"
            dst = f"v{destination}.{shape}" if shape[0].isdigit() else f"{shape}{destination}"
            rhs = (f"v2.{'s' if bits == 32 else 'd'}[{index}]" if index is not None
                   else f"v2.{shape}" if shape[0].isdigit() else f"{shape}2")
            assemblies.append(f"{operation} {dst}, {lhs}, {rhs}")
    source, obj, binary = (args.out / name for name in ("forms.s", "forms.o", "forms.bin"))
    source.write_text(".text\n" + "\n".join(assemblies) + "\n")
    subprocess.run([args.clang, "--target=aarch64-linux-gnu", "-c", str(source), "-o", str(obj)], check=True)
    subprocess.run([args.objcopy, "-O", "binary", "--only-section=.text", str(obj), str(binary)], check=True)
    encodings = binary.read_bytes()
    assert len(encodings) == len(assemblies) * 4
    rng = random.Random(0x20261111)
    manifest = []
    for form_id, (bits, lanes, indexed, operation, shape) in enumerate(forms):
        one = 0x3f800000 if bits == 32 else 0x3ff0000000000000
        minimum = 0x00800000 if bits == 32 else 0x0010000000000000
        infinity = 0x7f800000 if bits == 32 else 0x7ff0000000000000
        quiet = 0x00400000 if bits == 32 else 0x0008000000000000
        sign = 1 << (bits - 1)
        low = (65 if bits == 32 else 513) << (23 if bits == 32 else 52)
        high = (190 if bits == 32 else 1534) << (23 if bits == 32 else 52)
        pairs = [(one, one), (one + 1, one + 1), (sign, 0), (one, sign | one),
                 (low, low), (low - 1, low), (high - 1, high - 1), (high, one),
                 (one, 1), (minimum, one - 1), (minimum, one - (1 << (bits - 11 if bits == 64 else 22))),
                 (infinity - 1, one + 1), (infinity, 0), (infinity | 0x42, one),
                 (infinity | quiet | 0x42, infinity | 1), (minimum - 1, sign | minimum)]
        samples = [(control, sample, pair) for control in range(16) for sample, pair in enumerate(pairs)]
        samples += [(0, len(pairs) + i, (rng.getrandbits(bits), rng.getrandbits(bits))) for i in range(16)]
        for control, sample, pair in samples:
            destination = sample % 3
            assembly_index = form_id * 3 + destination
            state = oracle.initial_state()
            state["pc"] = 0x0400fffc
            state["gpr"][31] = 0x0600ff00
            state["flags"] = 0xa0000000
            state["fp_control"] = ((control & 3) << 22) | ((control & 4) << 22) | ((control & 8) << 22)
            state["fp_status"] = 0x08000000 | (0x9f if sample & 1 else 0)
            for reg in range(32):
                state["vectors"][reg][:16] = [(reg * 17 + i + 0xa5) & 255 for i in range(16)]
            for lane in range(128 // bits):
                # Early samples exercise entirely fast vectors; later samples
                # include one exceptional lane among ordinary finite lanes.
                left, right = pair if sample < 8 or lane == 0 else (one, one)
                state["vectors"][1][lane * bits // 8:(lane + 1) * bits // 8] = left.to_bytes(bits // 8, "little")
                state["vectors"][2][lane * bits // 8:(lane + 1) * bits // 8] = right.to_bytes(bits // 8, "little")
            if indexed is not None:
                offset = indexed * bits // 8
                state["vectors"][2][offset:offset + bits // 8] = pair[1].to_bytes(bits // 8, "little")
            case_id = len(manifest)
            case = {"isa": 2, "profile": "a64-user", "seed": 0x20261111, "id": case_id,
                    "form_id": f"inventory/arm64/fp-binary-{assembly_index}/{control}/{sample}",
                    "code_address": state["pc"], "terminal_pc": state["pc"] + 4,
                    "max_steps": 1, "code": encodings[assembly_index * 4:assembly_index * 4 + 4],
                    "initial": state, "memory": []}
            path = args.out / f"{case_id:05d}.svmcase"
            path.write_bytes(oracle.encode_case(case))
            manifest.append({"case": path.name, "assembly": assemblies[assembly_index],
                             "control": control, "sample": sample, "lanes": lanes})
    (args.out / "manifest.json").write_text(json.dumps({
        "cases": manifest, "forms": len(forms), "assembly_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "encoding_sha256": hashlib.sha256(encodings).hexdigest()}, indent=2) + "\n")
    print(json.dumps({"cases": len(manifest), "forms": len(forms)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
