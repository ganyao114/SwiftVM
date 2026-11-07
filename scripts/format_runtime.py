#!/usr/bin/env python3
"""Format the active C++ implementation without rewriting vendored sources."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[1]
ROOTS = ("source/base", "source/runtime", "source/aot", "source/launcher", "source/tests", "benchmarks")
TABLES = {
    "source/runtime/ir/opcodes.inc",
    "source/runtime/backend/riscv64/scalar_ops.inc",
    "source/runtime/backend/arm64/emit_ops.inc",
    "source/runtime/backend/riscv64/vector_ops.inc",
    "source/runtime/frontend/x86/opcodes.inc",
    "source/runtime/frontend/x86/detail/vex_opcodes.inc",
}
TABLE_STYLE = (
    "{BasedOnStyle: InheritParentConfig, ColumnLimit: 0, "
    "StatementMacros: [INST, X86_OP, X86_EXTRA_OP, VEX, RV_SCALAR_OP, RV_VECTOR_OP, A64_EMIT_OP]}"
)
AVX_STYLE = (
    "{BasedOnStyle: InheritParentConfig, StatementMacros: "
    "[AVX_BINARY, AVX_LANE, AVX_SIGNED, AVX_SCALAR, AVX_FMA_ONE, "
    "AVX_FMA_ORDER, AVX_FMA_KIND, AVX_FMA_ALTERNATE]}"
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report unformatted files")
    parser.add_argument(
        "--clang-format",
        default=os.environ.get("CLANG_FORMAT", "clang-format"),
        help="clang-format executable (version 21 is used for this checkpoint)",
    )
    args = parser.parse_args()
    files = sorted(
        path
        for root in ROOTS
        for path in (REPO / root).rglob("*")
        if path.suffix in {".cpp", ".h", ".inc"} and "externals" not in path.parts
    )
    failures = 0
    for path in files:
        command = [args.clang_format, "--sort-includes=false"]
        if path.relative_to(REPO).as_posix() in TABLES:
            command.append("--style=" + TABLE_STYLE)
        elif path.name == "semantics_avx.inc":
            command.append("--style=" + AVX_STYLE)
        if args.check:
            # clang-format 21 can report net-zero replacements for function-try
            # blocks in dry-run mode. Check the actual formatted bytes instead.
            original = path.read_bytes()
            formatted = subprocess.run(
                command + [str(path)], stdout=subprocess.PIPE, check=False
            )
            if formatted.returncode:
                print(
                    f"{path.relative_to(REPO)}: clang-format exited {formatted.returncode}",
                    file=sys.stderr,
                )
                failures += 1
            elif formatted.stdout != original:
                print(f"{path.relative_to(REPO)}: formatting differs", file=sys.stderr)
                failures += 1
        else:
            failures += subprocess.run(command + ["-i", str(path)], check=False).returncode != 0
    print(
        f"{'Checked' if args.check else 'Formatted'} {len(files)} C++ files; {failures} failures"
    )
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
