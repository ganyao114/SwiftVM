#!/usr/bin/env python3
"""Emit ARM scalar FP boundaries with legal, distinct operand registers.

All inputs are full SVMCASE records. Controls cover every rounding/FZ/DN
combination, and random bit patterns supplement directed numerical boundaries.
Encodings are Clang/LLVM-verified FADD/FSUB/FMUL/FDIV/FNMUL/FMADD and their VFP
counterparts. No comparison masks or oracle classifications are changed.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import random

import oracle


ENCODINGS = {
    "arm64": ("2028221e", "2038221e", "2008221e", "2018221e", "2088221e", "200c021f"),
    "arm32": ("040a32ee", "440a32ee", "040a22ee", "040a82ee", "440a22ee", "040aa2ee"),
    "thumb": ("32ee040a", "32ee440a", "22ee040a", "82ee040a", "22ee440a", "a2ee040a"),
}
OPERATIONS = ("add", "sub", "mul", "div", "nmul", "fma")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--random-cases", type=int, default=32)
    args = parser.parse_args()
    if args.random_cases < 0:
        parser.error("--random-cases must be nonnegative")
    args.out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(0x20261111)
    manifest = []
    for isa, encodings in ENCODINGS.items():
        for bits in (32, 64):
            one = 0x3f800000 if bits == 32 else 0x3ff0000000000000
            half = 0x3f000000 if bits == 32 else 0x3fe0000000000000
            minimum = 0x00800000 if bits == 32 else 0x0010000000000000
            infinity = 0x7f800000 if bits == 32 else 0x7ff0000000000000
            quiet = 0x00400000 if bits == 32 else 0x0008000000000000
            sign = 1 << (bits - 1)
            pairs = [(one, 1, 1), (sign | one, 1, sign | 1),
                     (minimum, one - 1, 0), (minimum, half, 0),
                     (infinity - 1, one + (one - half), 0), (infinity, 0, one),
                     (infinity | 0x42, one, one), (infinity | quiet | 0x42, infinity | 1, one),
                     (sign, 0, sign), (one, sign | one, 0),
                     (minimum - 1, minimum, 1), (one, one, 1),
                     (one, one, sign | 1), (0, 0, 0)]
            for operation, base in zip(OPERATIONS, encodings):
                code = bytearray.fromhex(base)
                if bits == 64:
                    code[2 if isa == "arm64" else 1 if isa == "arm32" else 3] |= (
                        0x40 if isa == "arm64" else 1)
                inputs = [(a, b, c, control) for a, b, c in pairs for control in range(16)]
                inputs += [(rng.getrandbits(bits), rng.getrandbits(bits), rng.getrandbits(bits), i & 15)
                           for i in range(args.random_cases)]
                for lhs, rhs, addend, control in inputs:
                    index = len(manifest)
                    state = oracle.initial_state()
                    state["pc"] = 0x0400fffc
                    state["gpr"][31 if isa == "arm64" else 13] = 0x0600ff00
                    state["flags"] = 0xa0000000 | (0 if isa == "arm64" else 0x10 | (32 if isa == "thumb" else 0))
                    state["fp_control"] = ((control & 3) << 22) | ((control & 4) << 22) | ((control & 8) << 22)
                    if isa == "arm64":
                        state["fp_status"] = 0x08000008
                    else:
                        state["fp_control"] |= 0x08000008
                    for vector in range(32 if isa == "arm64" else 16):
                        state["vectors"][vector][:16] = [(0xa5 + vector + i) & 255 for i in range(16)]
                    def lane(vector: int, value: int):
                        state["vectors"][vector][:bits // 8] = value.to_bytes(bits // 8, "little")
                    lane(1, lhs)
                    lane(2, rhs)
                    lane(3 if isa == "arm64" else 0, addend)
                    case = {"isa": 2 if isa == "arm64" else 3,
                            "profile": "a64-user" if isa == "arm64" else "armv7-user",
                            "form_id": f"inventory/{'arm64' if isa == 'arm64' else 'arm32'}/fp-{isa}-{operation}-{bits}/{index}",
                            "seed": 0x20261111, "id": index, "code_address": state["pc"],
                            "terminal_pc": 0x04010000, "max_steps": 1,
                            "code": code, "initial": state, "memory": []}
                    path = args.out / f"{index:05d}.svmcase"
                    path.write_bytes(oracle.encode_case(case))
                    manifest.append({"case": path.name, "isa": isa, "bits": bits,
                                     "operation": operation, "control": control,
                                     "lhs": hex(lhs), "rhs": hex(rhs), "addend": hex(addend)})
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"cases": len(manifest), "random_cases_per_form": args.random_cases}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
