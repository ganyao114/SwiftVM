#!/usr/bin/env python3
"""Generate the ARM32 encoding inventory and directed GE-data cases.

The form inventory enumerates every reviewed decode-table row with canonical
variable fields. Directed cases cover state that canonical fields hide: SEL
with explicit GE patterns and distinct operands, and packed byte operations
whose flags and data the plain inventory leaves at zero.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
import re
import shutil
import struct
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import oracle

CODE = 0x04000000
DATA = 0x06000000


class Rng:
    def __init__(self, seed: int):
        self.s = seed & ((1 << 64) - 1)

    def next(self) -> int:
        self.s = (self.s + 0x9E3779B97F4A7C15) & ((1 << 64) - 1)
        value = self.s
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & ((1 << 64) - 1)
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & ((1 << 64) - 1)
        return value ^ (value >> 31)


def parse_forms(table: Path, meta: Path) -> tuple[list[tuple[int, int, str, str, bool]], dict]:
    rows = []
    for line in table.read_text().splitlines():
        match = re.match(
            r"\s*\{0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u,\s*Opcode::(\w+),\s*"
            r"Group::(\w+),\s*(true|false)\}", line)
        if match:
            mask, expected, opcode, group, cond = match.groups()
            rows.append((int(mask, 16), int(expected, 16), opcode, group, cond == "true"))
    conditional = {}
    for line in meta.read_text().splitlines():
        match = re.match(r'\s*\{"(\w+)",\s*Group::(\w+),.*?,\s*(true|false),\s*(true|false)\}', line)
        if match:
            conditional[match.group(1)] = match.group(3) == "true"
    return rows, conditional


def form_state(thumb: bool, rng: Rng) -> dict:
    state = oracle.initial_state()
    for index in range(15):
        state["gpr"][index] = (DATA + 0x100 + 8 * index) & 0xFFFFFFFF
    state["gpr"][13] = DATA + 0xFF00
    state["gpr"][14] = DATA + 0xFF10
    for index in range(32):
        state["vectors"][index] = [rng.next() & 0xFF for _ in range(32)]
    state["pc"] = CODE
    state["flags"] = 0x10 | (0x20 if thumb else 0)
    return state


def code_bytes(thumb: bool, word: int, length: int = 4) -> bytes:
    if thumb and length == 4:
        return struct.pack("<HH", word >> 16, word & 0xFFFF)
    return word.to_bytes(length, "little")


def memory_region(seed: int) -> list[dict]:
    rng = Rng(seed)
    return [{"address": DATA, "size": 65536, "protection": 3,
             "bytes": [rng.next() & 0xFF for _ in range(256)]}]


def directed_case(form_id: str, seed: int, thumb: bool, word: int, gpr: dict,
                  ge: int) -> dict:
    state = oracle.initial_state()
    for index, value in gpr.items():
        state["gpr"][index] = value & 0xFFFFFFFF
    state["pc"] = CODE
    state["flags"] = 0x10 | (0x20 if thumb else 0) | (ge << 16)
    return {
        "isa": 3,
        "profile": "armv7-user",
        "form_id": form_id,
        "seed": seed,
        "id": seed,
        "code_address": CODE,
        "terminal_pc": CODE + 4,
        "max_steps": 1,
        "code": list(code_bytes(thumb, word)),
        "initial": state,
        "memory": memory_region(seed),
    }


def relocate_for_vm(case: dict) -> dict:
    """Place the instruction so it ends exactly at the 64 KiB source boundary.

    The VM correctness path requires the terminal PC to lie outside executable
    source; ending at the granule edge satisfies that without extra code.
    """
    length = len(case["code"])
    address = 0x04010000 - length
    moved = dict(case)
    moved["code_address"] = address
    moved["terminal_pc"] = address + length
    moved["initial"] = dict(case["initial"], pc=address)
    return moved


def directed_cases(base_count: int, cases_dir: Path, vm_dir: Path | None) -> list[dict]:
    # Encodings from arm-linux-gnueabihf-as; see the form ids for operands.
    sel_r012 = {False: 0xE6810FB2, True: 0xFAA1F082}
    sel_r135 = {False: 0xE6831FB5, True: 0xFAA3F185}
    uadd8_r312 = {False: 0xE6513F92, True: 0xFA81F342}
    usad8_r012 = {False: 0xE780F211, True: 0xFB71F002}
    usada8_r0123 = {False: 0xE7803211, True: 0xFB713002}
    cases = []

    def add(name: str, opcode: str, thumb: bool, word: int, gpr: dict, ge: int,
            note: str):
        index = base_count + len(cases)
        group = "Thumb32" if thumb else "Arm"
        path = cases_dir / f"{index:04d}.svmcase"
        form_id = f"inventory/directed/arm32/{name}/{group.lower()}"
        case = directed_case(form_id, index, thumb, word, gpr, ge)
        path.write_bytes(oracle.encode_case(case))
        entry = {"index": index, "opcode": opcode, "group": group,
                 "thumb": thumb, "length": 4,
                 "code_hex": code_bytes(thumb, word).hex(),
                 "conditional_vfp": False, "kind": "directed",
                 "note": note, "case": str(path)}
        if vm_dir is not None:
            vm_path = vm_dir / f"{index:04d}.svmcase"
            vm_path.write_bytes(oracle.encode_case(relocate_for_vm(case)))
            entry["vm_case"] = str(vm_path)
        cases.append(entry)

    for thumb in (False, True):
        group = "T32" if thumb else "A32"
        for label, ge in (("ge-0", 0x0), ("ge-f", 0xF), ("ge-5", 0x5),
                          ("ge-a", 0xA), ("ge-3", 0x3), ("ge-c", 0xC)):
            add(f"sel-ge/{group}/{label}", "SEL", thumb, sel_r012[thumb],
                {1: 0x11111111, 2: 0x22222222}, ge,
                f"GE=0b{ge:04b}; Rn=0x11111111 Rm=0x22222222")
        add(f"sel-distinct/{group}", "SEL", thumb, sel_r135[thumb],
            {3: 0x01234567, 5: 0x89ABCDEF}, 0xA,
            "GE=0b1010; Rn=r3 Rm=r5 with distinct byte patterns")
        add(f"sel-bytes/{group}", "SEL", thumb, sel_r012[thumb],
            {1: 0x00112233, 2: 0xFFEEDDCC}, 0x6,
            "GE=0b0110; per-byte operand selection")

        for label, lhs, rhs in (("zero", 0x00000000, 0xFFFFFFFF),
                                ("ones", 0xFFFFFFFF, 0xFFFFFFFF),
                                ("strlen", 0x11220033, 0xFFFFFFFF),
                                ("half", 0x80808080, 0x80808080),
                                ("mixed", 0x00FF00FF, 0x01010101)):
            add(f"uadd8/{group}/{label}", "UADD8", thumb, uadd8_r312[thumb],
                {1: lhs, 2: rhs}, 0xA,
                f"Rn={lhs:#010x} Rm={rhs:#010x}; initial GE=0b1010")
        for label, lhs, rhs in (("same", 0x04030201, 0x04030201),
                                ("max", 0xFFFFFFFF, 0x00000000),
                                ("mixed", 0x00112233, 0xFFEEDDCC)):
            add(f"usad8/{group}/{label}", "USAD8", thumb, usad8_r012[thumb],
                {1: lhs, 2: rhs}, 0xA,
                f"Rn={lhs:#010x} Rm={rhs:#010x}; GE must be preserved")
        for label, lhs, rhs, accumulator in (
                ("accum", 0x0F0E0D0C, 0x01020304, 0x7FFFFFFF),
                ("wrap", 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF)):
            add(f"usada8/{group}/{label}", "USADA8", thumb, usada8_r0123[thumb],
                {1: lhs, 2: rhs, 3: accumulator}, 0xA,
                f"Rn={lhs:#010x} Rm={rhs:#010x} Ra={accumulator:#010x}; GE preserved")
    return cases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="output directory for cases/ and manifest.json")
    parser.add_argument("--table", type=Path,
                        default=REPO / "source/runtime/frontend/arm32/decode/generated_tables.inc")
    parser.add_argument("--meta", type=Path,
                        default=REPO / "source/runtime/frontend/arm32/decode/generated_metadata.inc")
    parser.add_argument("--skip-base", action="store_true",
                        help="emit only the directed GE-data cases")
    parser.add_argument("--vm", action="store_true",
                        help="also emit cases relocated to the 64 KiB source edge for VM backends")
    args = parser.parse_args()
    args.root = args.root.resolve()

    rows, conditional = parse_forms(args.table, args.meta)
    cases_dir = args.root / "cases"
    shutil.rmtree(cases_dir, ignore_errors=True)
    cases_dir.mkdir(parents=True)
    vm_dir = args.root / "vm-cases" if args.vm else None
    if vm_dir is not None:
        shutil.rmtree(vm_dir, ignore_errors=True)
        vm_dir.mkdir(parents=True)
    (args.root / "manifest").mkdir(parents=True, exist_ok=True)

    manifest = []
    if not args.skip_base:
        for index, (mask, expected, opcode, group, cond) in enumerate(rows):
            thumb = group in ("Thumb16", "Thumb32")
            length = 2 if group == "Thumb16" else 4
            rng = Rng(0x9E3779B97F4A7C15 ^ index)
            word = expected
            if group in ("Arm", "VFP", "NEON") and conditional.get(opcode, True) \
                    and (mask & 0xF0000000) != 0xF0000000:
                word = (word & ~0xF0000000) | 0xE0000000
            code = code_bytes(thumb, word, length)
            case = {
                "isa": 3, "profile": "armv7-user",
                "form_id": f"inventory/arm32/{group}/{opcode}/{index}",
                "seed": index, "id": index, "code_address": CODE,
                "terminal_pc": CODE + length, "max_steps": 1,
                "code": list(code), "initial": form_state(thumb, rng),
                "memory": [{"address": DATA, "size": 65536, "protection": 3,
                            "bytes": [rng.next() & 0xFF for _ in range(256)]}],
            }
            path = cases_dir / f"{index:04d}.svmcase"
            path.write_bytes(oracle.encode_case(case))
            entry = {"index": index, "opcode": opcode, "group": group,
                     "thumb": thumb, "length": length, "code_hex": code.hex(),
                     "conditional_vfp": cond, "kind": "form", "case": str(path)}
            if vm_dir is not None:
                vm_path = vm_dir / f"{index:04d}.svmcase"
                vm_path.write_bytes(oracle.encode_case(relocate_for_vm(case)))
                entry["vm_case"] = str(vm_path)
            manifest.append(entry)

    manifest.extend(directed_cases(len(manifest), cases_dir, vm_dir))
    (args.root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("cases", len(manifest),
          "kinds", Counter(entry["kind"] for entry in manifest),
          "directed opcodes",
          Counter(entry["opcode"] for entry in manifest if entry["kind"] == "directed"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
