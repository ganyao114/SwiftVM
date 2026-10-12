#!/usr/bin/env python3
"""Syntax-check shared integer vector emitters using an existing Ninja build.

Example (inside WSL/Linux):
  python3 scripts/check-vecdot-cross.py --build-dir /root/swiftvm-build-x64

This reads `ninja -t commands swift_runtime`; Ninja does not build anything.
Compiler checks use -fsyntax-only and do not create object/dependency files.
It checks C++ target syntax, not linking, generated instructions or semantics.
"""

import argparse
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "arm64": (
        "source/runtime/backend/arm64/jit/translator_alu_vec_int.cpp",
        "source/runtime/backend/arm64/jit/translator_flags.cpp",
        "source/runtime/backend/reg_alloc.cpp",
    ),
    "riscv64": (
        "source/runtime/backend/riscv64/jit/translator_vector_integer.cpp",
        "source/runtime/backend/riscv64/jit/jit_context_cfg.cpp",
        "source/runtime/backend/riscv64/jit/jit_context_liveness.cpp",
    ),
}
ARCH_DEFINITION = re.compile(
    r"^(?:SWIFT_HAS_(?:X64|ARM64|RISCV64)_BACKEND|"
    r"VIXL_(?:TARGET_ARCH|INCLUDE_TARGET)_.+|"
    r"(?:HOST|TARGET|ARCH)_(?:X86_64|X64|ARM64|AARCH64|RISCV64)|"
    r"__(?:x86_64__|amd64__|aarch64__|riscv|riscv_xlen))$")
PAIRED_OPTIONS = {"-o", "-MF", "-MT", "-MQ", "-MJ", "-march", "-mtune", "-mcpu", "-mabi", "--target", "-target"}
DEPENDENCY_OPTIONS = {"-MD", "-MMD", "-MP", "-MG", "-M", "-MM"}


def runtime_command(build_dir, ninja):
    result = subprocess.run([ninja, "-C", str(build_dir), "-t", "commands", "swift_runtime"],
                            check=True, text=True, capture_output=True)
    for line in result.stdout.splitlines():
        args = shlex.split(line)
        if "-c" not in args or not any("CMakeFiles/swift_runtime.dir/" in item for item in args):
            continue
        index = args.index("-c")
        if index + 1 >= len(args) or Path(args[index + 1]).suffix not in (".cpp", ".cc", ".cxx"):
            continue
        if any(item in ("&&", ";", "|", "||") for item in args):
            raise ValueError("runtime compile command contains shell control operators")
        # Compiler launchers such as ccache must not carry across target tools.
        compiler = next((n for n, item in enumerate(args[:index])
                         if re.search(r"(?:^|/)(?:[^/]*-)?(?:c\+\+|g\+\+|clang\+\+)(?:-[0-9.]+)?$", item)), None)
        if compiler is None:
            raise ValueError("could not identify C++ compiler in runtime command")
        return args[compiler + 1:], args[index + 1]
    raise ValueError("Ninja did not return a swift_runtime C++ compilation command")


def cross_flags(args, original_source):
    result = []
    index = 0
    while index < len(args):
        flag = args[index]
        index += 1
        if flag in PAIRED_OPTIONS:
            if index >= len(args):
                raise ValueError(f"missing argument for {flag}")
            index += 1
            continue
        if flag == "-c" or flag == original_source or flag in DEPENDENCY_OPTIONS:
            continue
        # Native machine tuning/ISA switches and target triples cannot be
        # reused by a cross compiler. Preserve all include paths and all
        # architecture-independent defines/flags, including quoted values.
        if flag.startswith(("-march=", "-mtune=", "-mcpu=", "-mabi=", "--target=", "-m")):
            continue
        if flag.startswith(("-MF", "-MT", "-MQ", "-MJ")):
            continue
        if flag in ("-D", "-U"):
            if index >= len(args):
                raise ValueError(f"missing argument for {flag}")
            definition = args[index]
            index += 1
            if not ARCH_DEFINITION.fullmatch(definition.split("=", 1)[0]):
                result.extend((flag, definition))
            continue
        if flag.startswith(("-D", "-U")) and ARCH_DEFINITION.fullmatch(flag[2:].split("=", 1)[0]):
            continue
        if flag.startswith("@"):
            raise ValueError("response-file compile commands need a CMake compile_commands.json configuration")
        result.append(flag)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True, help="existing x64 Ninja build directory")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--arm64-compiler", default="aarch64-linux-gnu-g++")
    parser.add_argument("--riscv64-compiler", default="riscv64-linux-gnu-g++")
    parser.add_argument("--arch", choices=tuple(SOURCES), action="append", help="check one architecture; default checks both")
    parser.add_argument("--dry-run", action="store_true", help="print exact commands without invoking compilers")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    if not (build_dir / "build.ninja").is_file():
        parser.error(f"not a configured Ninja build directory: {build_dir}")
    try:
        original, source = runtime_command(build_dir, args.ninja)
        flags = cross_flags(original, source)
        if not any(flag.startswith("-I") or flag == "-isystem" for flag in flags):
            raise ValueError("runtime command has no include paths")
        for arch in dict.fromkeys(args.arch or SOURCES):
            compiler = args.arm64_compiler if arch == "arm64" else args.riscv64_compiler
            for source_file in SOURCES[arch]:
                command = [compiler, *flags, "-fsyntax-only", str(ROOT / source_file)]
                print(f"{arch}: {shlex.join(command)}", flush=True)
                if not args.dry_run:
                    if not shutil.which(compiler):
                        raise ValueError(f"cross compiler unavailable: {compiler}")
                    subprocess.run(command, cwd=build_dir, check=True)
                    print(f"PASS {arch} {Path(source_file).name} C++ syntax", flush=True)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
