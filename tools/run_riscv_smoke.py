#!/usr/bin/env python3
"""Execute generated RV64 functions in a freestanding Linux ELF under QEMU."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile


BASELINE_CPU = "rv64,c=false,v=false,zba=false,zbb=false,zbs=false,zbc=false"
SAVED_REGISTERS = [f"s{i}" for i in range(12)]


def word(value):
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]{16}", value):
        raise ValueError(f"Expected a 64-bit hexadecimal word, got {value!r}")
    return int(value, 16)


def words(value, length):
    if not isinstance(value, list) or len(value) != length:
        raise ValueError(f"Expected exactly {length} words")
    return [word(item) for item in value]


def load_bundle(directory):
    directory = Path(directory).resolve()
    manifest = json.loads((directory / "manifest.json").read_text())
    if (manifest.get("format_version"), manifest.get("isa"), manifest.get("abi")) != (1, "rv64g", "lp64d"):
        raise ValueError("Unsupported bundle format, ISA or ABI")
    functions = manifest.get("functions")
    cases = manifest.get("cases")
    if not isinstance(functions, list) or not 1 <= len(functions) <= 512:
        raise ValueError("Bundle must contain 1..512 generated functions")
    if not isinstance(cases, list) or not 1 <= len(cases) <= 4096:
        raise ValueError("Bundle must contain 1..4096 execution cases")
    by_name = {}
    cookies = set()
    for function in functions:
        name = function.get("name")
        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", name) or name in by_name:
            raise ValueError("Invalid or duplicate function name")
        if function.get("file") != name + ".bin":
            raise ValueError("Function must use its own relative .bin filename")
        path = (directory / function["file"]).resolve()
        if path.parent != directory:
            raise ValueError("Function file is outside the bundle")
        size = function.get("size")
        if type(size) is not int or size <= 0 or size % 4 != 0 or path.stat().st_size != size:
            raise ValueError(f"Invalid RV64G code size for {name}")
        cookie = function.get("cookie")
        if type(cookie) is not int or not 1 <= cookie <= 512 or cookie in cookies:
            raise ValueError("Execution cookies must be unique and nonzero")
        cookies.add(cookie)
        by_name[name] = dict(function, path=path)
    names = set()
    used = set()
    normalized = []
    for case in cases:
        name = case.get("name")
        if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9_.-]+", name) or name in names:
            raise ValueError("Invalid or duplicate execution case name")
        function = case.get("function")
        if function not in by_name:
            raise ValueError(f"Unknown function for case {name}")
        names.add(name)
        used.add(function)
        normalized.append(dict(case, args=words(case.get("args"), 2),
                               memory=words(case.get("memory"), 4),
                               expected_result=word(case.get("expected_result")),
                               expected_memory=words(case.get("expected_memory"), 4)))
    if used != set(by_name):
        raise ValueError("Every generated function must have an execution case")
    return by_name, normalized


def write_message(lines, label, message):
    # Linux write(1, message, length); a short/failed write fails the run.
    lines.extend(["    li a0, 1", f"    la a1, {label}", f"    li a2, {len(message.encode())}",
                  "    li a7, 64", "    ecall", f"    li t0, {len(message.encode())}",
                  f"    beq a0, t0, .Lwritten_{label}", "    li a0, 2", "    li a7, 93",
                  "    ecall", f".Lwritten_{label}:"])


def assembly(functions, cases, negative=False):
    lines = [".option norelax", ".text", ".globl _start", ".type _start, @function", "_start:",
             "    la t0, saved_sp", "    sd sp, 0(t0)"]
    for index, reg in enumerate(SAVED_REGISTERS):
        lines.append(f"    li {reg}, {0x5100 + index}")
    messages = []
    for index, case in enumerate(cases):
        fail = f".Lfail_{index}"
        lines.extend([f"    li a0, 0x{case['args'][0]:016x}",
                      f"    li a1, 0x{case['args'][1]:016x}", f"    la a2, memory_{index}",
                      "    li t6, 0", f"    call fn_{case['function']}"])
        expected = case["expected_result"] ^ (1 if negative and index == 0 else 0)
        lines.extend([f"    li t0, 0x{expected:016x}", f"    bne a0, t0, {fail}",
                      f"    li t0, {functions[case['function']]['cookie']}", f"    bne t6, t0, {fail}",
                      "    la t0, saved_sp", "    ld t1, 0(t0)", f"    bne sp, t1, {fail}",
                      "    andi t0, sp, 15", f"    bnez t0, {fail}"])
        for reg_index, reg in enumerate(SAVED_REGISTERS):
            lines.extend([f"    li t0, {0x5100 + reg_index}", f"    bne {reg}, t0, {fail}"])
        lines.append(f"    la t2, memory_{index}")
        for memory_index, value in enumerate(case["expected_memory"]):
            lines.extend([f"    ld t0, {memory_index * 8}(t2)", f"    li t1, 0x{value:016x}",
                          f"    bne t0, t1, {fail}"])
        passed = f"PASS {case['name']}\n"
        failed = f"FAIL {case['name']}\n"
        write_message(lines, f"passed_{index}", passed)
        lines.extend([f"    j .Lnext_{index}", f"{fail}:"])
        write_message(lines, f"failed_{index}", failed)
        lines.extend(["    li a0, 1", "    li a7, 93", "    ecall", f".Lnext_{index}:"])
        messages.extend([(f"passed_{index}", passed), (f"failed_{index}", failed)])
    complete = f"OK {len(cases)}\n"
    write_message(lines, "complete", complete)
    lines.extend(["    li a0, 0", "    li a7, 93", "    ecall"])
    messages.append(("complete", complete))
    for name, function in functions.items():
        lines.extend([".p2align 2", f".type fn_{name}, @function", f"fn_{name}:",
                      f"    .incbin {json.dumps(str(function['path']), ensure_ascii=False)}"])
    lines.append(".section .rodata")
    for label, message in messages:
        lines.extend([f"{label}:", f"    .ascii {json.dumps(message)}"])
    lines.extend([".data", ".p2align 3", "saved_sp:", "    .quad 0"])
    for index, case in enumerate(cases):
        lines.extend([f"memory_{index}:", "    .quad " + ", ".join(f"0x{v:016x}" for v in case["memory"])])
    lines.append('.section .note.GNU-stack,"",@progbits')
    return "\n".join(lines) + "\n"


def validate_execution(returncode, output, cases, negative=False):
    if not cases:
        raise RuntimeError("RV64 execution requires at least one case")
    if negative:
        if returncode != 1 or output.splitlines() != [f"FAIL {cases[0]['name']}"]:
            raise RuntimeError("Negative control did not reject the deliberately wrong result")
        return 0
    expected = [f"PASS {case['name']}" for case in cases] + [f"OK {len(cases)}"]
    if returncode != 0 or output.splitlines() != expected:
        passed = passed_prefix(output, cases)
        next_case = cases[passed]["name"] if passed < len(cases) else "completion marker"
        raise RuntimeError(f"RV64 execution failed: exit={returncode}, passed={passed}/{len(cases)}, "
                           f"next={next_case}; see positive_run.stdout/stderr")
    return len(cases)


def passed_prefix(output, cases):
    count = 0
    for line, case in zip(output.splitlines(), cases):
        if line != f"PASS {case['name']}":
            break
        count += 1
    return count


def require_tool(name):
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"Required executable not found: {name}")
    return str(Path(path).resolve())


def run_job(label, command, work, timeout, record):
    stdout = work / (label + ".stdout")
    stderr = work / (label + ".stderr")
    expired = False
    def setup():
        if os.name == "posix":
            import resource
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with stdout.open("wb") as out, stderr.open("wb") as err:
        proc = subprocess.Popen(command, cwd=work, stdout=out, stderr=err,
                                start_new_session=True, preexec_fn=setup if os.name == "posix" else None)
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            expired = True
            if os.name == "posix":
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            else:
                proc.kill()
            code = proc.wait()
    record["jobs"][label] = {"command": command, "returncode": code, "timed_out": expired,
                              "stdout": str(stdout), "stderr": str(stderr)}
    if expired:
        raise RuntimeError(f"{label} exceeded {timeout} seconds")
    return code, stdout.read_text(errors="replace")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--emitter", help="Native swift_riscv_emit executable")
    source.add_argument("--bundle", type=Path, help="Previously emitted bundle, for execution on another host")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu", default="qemu-riscv64")
    parser.add_argument("--cpu", default=BASELINE_CPU)
    parser.add_argument("--artifact-root", type=Path, help="Parent of a new directory for each run")
    parser.add_argument("--timeout", type=float, default=5, help="Seconds per QEMU run")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if args.artifact_root:
        args.artifact_root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="swiftvm-riscv-", dir=args.artifact_root)).resolve()
    record = {"format_version": 1, "status": "failed", "cpu": args.cpu,
              "executed_cases": 0, "jobs": {}, "artifacts": str(work)}
    try:
        clang = require_tool(args.clang)
        qemu = require_tool(args.qemu)
        for name, path in (("clang", clang), ("qemu", qemu)):
            code, version = run_job(name + "_version", [path, "--version"], work, 5, record)
            if code != 0:
                raise RuntimeError(f"Cannot query {name} version")
            record[name] = {"path": path, "version": version.strip()}
        if args.emitter:
            emitter = require_tool(args.emitter)
            directory = work / "bundle"
            code, _ = run_job("emit", [emitter, str(directory)], work, 15, record)
            if code != 0:
                raise RuntimeError("Native emitter failed")
            record["emitter_sha256"] = hashlib.sha256(Path(emitter).read_bytes()).hexdigest()
        else:
            directory = args.bundle.resolve()
        functions, cases = load_bundle(directory)
        record.update(bundle=str(directory), functions=len(functions), expected_cases=len(cases),
                      manifest_sha256=hashlib.sha256((directory / "manifest.json").read_bytes()).hexdigest(),
                      code_sha256={name: hashlib.sha256(fn["path"].read_bytes()).hexdigest()
                                   for name, fn in functions.items()})
        for negative in (False, True):
            label = "negative" if negative else "positive"
            source_file = work / (label + ".S")
            source_file.write_text(assembly(functions, cases, negative))
            elf = work / (label + ".elf")
            command = [clang, "--target=riscv64-linux-gnu", "-march=rv64g", "-mabi=lp64d",
                       "-nostdlib", "-static", "-fuse-ld=lld", "-Wl,-e,_start", str(source_file), "-o", str(elf)]
            code, _ = run_job(label + "_build", command, work, 30, record)
            if code != 0:
                raise RuntimeError(f"{label} ELF build failed; see {label}_build.stderr")
            code, output = run_job(label + "_run", [qemu, "-cpu", args.cpu, str(elf)],
                                   work, args.timeout, record)
            if not negative:
                record["executed_cases"] = passed_prefix(output, cases)
            executed = validate_execution(code, output, cases, negative)
            if not negative:
                record["executed_cases"] = executed
            else:
                record["negative_control_rejected"] = True
        record["status"] = "passed"
        print(f"Passed {len(cases)} RV64 scenarios across {len(functions)} functions; negative control rejected.")
        returncode = 0
    except (OSError, ValueError, KeyError, TypeError, AttributeError, RuntimeError) as error:
        record["error"] = str(error)
        print(f"RV64 smoke test failed: {error}", file=sys.stderr)
        returncode = 1
    finally:
        (work / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        print(f"Artifacts: {work}", flush=True)
    return returncode


if __name__ == "__main__":
    raise SystemExit(main())
