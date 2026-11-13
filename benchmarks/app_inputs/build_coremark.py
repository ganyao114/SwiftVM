#!/usr/bin/env python3
"""Build unchanged official CoreMark plus a syscall-only Linux x86-64 port.

Does not run a guest, emulator, backend, pass, or performance measurement.
The output ELF must be shared unchanged by NewR and FEX comparisons.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

PIN = "1f483d5b8316753a742cbf5590caf5bd0a4e4777"
CORE = ["core_main.c", "core_list_join.c", "core_matrix.c", "core_state.c", "core_util.c"]
NDK = Path("/Users/swift/Library/Android/sdk/ndk/29.0.13846066/toolchains/llvm/prebuilt/darwin-x86_64/bin")


def identity(path):
    return {"path": str(path.resolve()), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def run(argv, env):
    completed = subprocess.run([str(x) for x in argv], check=True, env=env, capture_output=True, text=True)
    return completed.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("/Volumes/Data2/swiftvm-middle-build-cache/app-inputs/coremark"))
    parser.add_argument("--out", type=Path, default=Path("/Volumes/Data2/swiftvm-middle-build-cache/app-inputs/coremark-linux-x86_64"))
    parser.add_argument("--toolchain", type=Path, default=NDK)
    parser.add_argument("--warmup", action="store_true", help="Run one complete warmup before the timed CoreMark invocation")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    temporary = args.out / "tmp"; temporary.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temporary))
    source_revision = run(["git", "-C", args.source, "rev-parse", "HEAD"], env).strip()
    if source_revision != PIN:
        raise SystemExit("official source revision mismatch; do not silently change the workload")
    changes = run(["git", "-C", args.source, "status", "--porcelain", "--", *CORE, "coremark.h", "LICENSE.md"], env)
    if changes:
        raise SystemExit("official core files/license have local changes")
    port = Path(__file__).resolve().parent / "coremark_linux"
    flags = ["--target=x86_64-unknown-linux-gnu", "-O2", "-g0", "-ffreestanding", "-fno-stack-protector",
             "-fno-pie", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-std=c11",
             "-DPERFORMANCE_RUN=1", "-DTOTAL_DATA_SIZE=2000", "-I" + str(port), "-I" + str(args.source)]
    commands, objects = [], []
    sources = [*(args.source / name for name in CORE), port / "core_portme.c", port / "start_x86_64.S"]
    warmup = args.out / "warmup.c"
    if args.warmup:
        warmup.write_text("int coremark_main(int, char **);\n"
                          "int main(int argc, char **argv) {\n"
                          "    int result = coremark_main(argc, argv);\n"
                          "    return result ? result : coremark_main(argc, argv);\n}\n")
        sources.append(warmup)
    for source in sources:
        obj = args.out / (source.stem + ".o")
        alias = ["-Dmain=coremark_main"] if args.warmup and source.name == "core_main.c" else []
        argv = [args.toolchain / "clang", *flags, *alias, "-c", source, "-o", obj]
        run(argv, env); commands.append([str(x) for x in argv]); objects.append(obj)
    elf = args.out / "coremark-linux-x86_64"
    argv = [args.toolchain / "ld.lld", "-m", "elf_x86_64", "--static", "--entry=_start", "--build-id=sha1", "-z", "noexecstack", "-o", elf, *objects]
    run(argv, env); commands.append([str(x) for x in argv])
    shutil.copyfile(args.source / "LICENSE.md", args.out / "LICENSE.md")
    disassembly = run([args.toolchain / "llvm-objdump", "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", elf], env)
    (args.out / "disassembly.txt").write_text(disassembly)
    mnemonics, functions = Counter(), {}
    function = ""
    for line in disassembly.splitlines():
        label = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if label:
            function = label.group(1); functions[function] = Counter()
        instruction = re.match(r"^\s*[0-9a-f]+:\s+([a-z][a-z0-9.]*)\b", line)
        if instruction:
            name = instruction.group(1); mnemonics[name] += 1
            functions.setdefault(function, Counter())[name] += 1
    manifest = {"kind": "official_coremark_linux_syscall_port", "source_repository": "https://github.com/eembc/coremark",
                "source_commit": source_revision, "source_files_unchanged": True,
                "sources": [identity(args.source / name) for name in CORE + ["coremark.h", "LICENSE.md"]],
                "port": [identity(port / name) for name in ("core_portme.h", "core_portme.c", "start_x86_64.S")],
                "builder": identity(Path(__file__)), "warmup": identity(warmup) if args.warmup else None,
                "compiler_version": run([args.toolchain / "clang", "--version"], env).splitlines()[0],
                "linker_version": run([args.toolchain / "ld.lld", "--version"], env).strip(), "commands": commands,
                "binary": identity(elf), "elf": run([args.toolchain / "llvm-readelf", "-h", "-l", elf], env),
                "configuration": {"optimization": "O2; no LTO/PGO; no vectorization/ISA disabling options", "HAS_FLOAT": 0,
                                  "timing": "CLOCK_MONOTONIC microsecond ticks; official integer seconds report", "SEED_METHOD": "SEED_ARG",
                                  "MEM_METHOD": "MEM_MALLOC port backed by 64KiB single-context arena", "TOTAL_DATA_SIZE": 2000, "MULTITHREAD": 1,
                                  "syscalls": {"write": 1, "clock_gettime": 228, "exit": 60}},
                "example_argv": [str(elf), "0x0", "0x0", "0x66", "2000", "7", "1", "2000"],
                "validation_example": [str(elf), "0x3415", "0x3415", "0x66", "2000", "7", "1", "2000"],
                "static_instruction_census": {"instructions": sum(mnemonics.values()), "mnemonics": dict(mnemonics.most_common()),
                                              "functions": {name: dict(value.most_common()) for name, value in functions.items()}},
                "qualification": "Built, not run. Static census is not dynamic cold-compile coverage. CRC and >=10s official validity remain unverified. Same binary required for FEX/NewR."}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"out": str(args.out), "binary": manifest["binary"], "static_instructions": sum(mnemonics.values()), "mnemonics": mnemonics.most_common()}))


if __name__ == "__main__":
    main()
