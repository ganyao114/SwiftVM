#!/usr/bin/env python3
"""Pre-registered quiet-window runner for the placement 2x2 experiment."""

import argparse
import json
import os
import re
import signal
import statistics
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSLATOR = ROOT / "build-master/source/translator/linux/svm_translator_linux"
COREMARK = Path("/Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64")
COMMAND = [str(TRANSLATOR), str(COREMARK), "0", "0", "0x66", "200000",
           "7", "1", "2000"]
RUN_TIMEOUT_S = 15.0
ARMS = {
    "cell1": {"SVM_INT_IMM_FOLD": "0", "SVM_PLACEMENT_PAD": "0",
              "SVM_RA_HOME_PERM": "0"},
    "cell2": {"SVM_INT_IMM_FOLD": "1", "SVM_PLACEMENT_PAD": "1",
              "SVM_RA_HOME_PERM": "0"},
    "cell3": {"SVM_INT_IMM_FOLD": "1", "SVM_PLACEMENT_PAD": "0",
              "SVM_RA_HOME_PERM": "0"},
    "cell4": {"SVM_INT_IMM_FOLD": "1", "SVM_PLACEMENT_PAD": "1",
              "SVM_RA_HOME_PERM": "1"},
}
FOREIGN = re.compile(
    r"SwiftVM-w6[78]/build/.+svm_translator_linux|target/debug/winemu|"
    r"Python.+(?:test_|-m unittest|winemu-vcpu2)|qemu-system-aarch64|"
    r"/rustc(?: |$)|/clang(?:\+\+)?(?: |$)|codex.+vcpu2"
)


def foreign_cpu() -> float:
    result = subprocess.run(["ps", "-Ao", "pcpu=,command="], text=True,
                            stdout=subprocess.PIPE, check=True)
    total = 0.0
    for line in result.stdout.splitlines():
        fields = line.strip().split(maxsplit=1)
        if len(fields) == 2 and FOREIGN.search(fields[1]):
            total += float(fields[0])
    return total


def wait_for_quiet(seconds: int, timeout: int) -> dict:
    start = time.monotonic()
    quiet_start = None
    samples = []
    while time.monotonic() - start < timeout:
        load1 = os.getloadavg()[0]
        foreign = foreign_cpu()
        good = load1 < 20.0 and foreign < 20.0
        now = time.monotonic()
        samples.append({"elapsed": now - start, "load1": load1,
                        "foreign_cpu": foreign, "good": good})
        if good:
            quiet_start = quiet_start or now
            if now - quiet_start >= seconds:
                return {"accepted": True, "wait_s": now - start,
                        "samples": samples}
        else:
            quiet_start = None
        time.sleep(1)
    return {"accepted": False, "wait_s": time.monotonic() - start,
            "samples": samples}


def run_one(arm: str, timeout_s: float = RUN_TIMEOUT_S) -> dict:
    env = os.environ.copy()
    env.pop("SVM_JIT_CACHE", None)
    env.pop("SVM_EXEC_PROF", None)
    env.update(ARMS[arm])
    before = {"load1": os.getloadavg()[0], "foreign_cpu": foreign_cpu()}
    start = time.perf_counter()
    timed_out = False
    with subprocess.Popen(["nice", "-n", "15", *COMMAND], env=env, text=True,
                          start_new_session=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as proc:
        try:
            try:
                output, _ = proc.communicate(timeout=timeout_s)
            except subprocess.TimeoutExpired:
                timed_out = True
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                output, _ = proc.communicate()
        except BaseException:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
            raise
    wall = time.perf_counter() - start
    match = re.search(r"CoreMark 1\.0\s*:\s*([0-9.]+)", output)
    return {
        "arm": arm, "wall_s": wall, "rc": proc.returncode,
        "timed_out": timed_out,
        "iter_s": float(match.group(1)) if match else None,
        "oracle": "crcfinal      : 0x4983" in output and
                  "Correct operation validated" in output,
        "before": before,
        "after": {"load1": os.getloadavg()[0], "foreign_cpu": foreign_cpu()},
        "output_tail": output[-600:],
    }


def spread(records: list[dict]) -> float:
    values = [record["wall_s"] for record in records]
    return (max(values) - min(values)) / statistics.median(values)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--quiet-seconds", type=int, default=80)
    parser.add_argument("--quiet-timeout", type=int, default=600)
    parser.add_argument("--preflight", type=int, default=4)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--quiet-only", action="store_true")
    args = parser.parse_args()
    payload = {"quiet": wait_for_quiet(args.quiet_seconds, args.quiet_timeout),
               "preflight": {}, "rounds": []}
    if not payload["quiet"]["accepted"]:
        payload["accepted"] = False
        payload["reason"] = "no continuous quiet window"
        args.output.write_text(json.dumps(payload, indent=2))
        raise SystemExit(2)
    if args.quiet_only:
        payload["accepted"] = True
        payload["reason"] = "quiet gate only"
        args.output.write_text(json.dumps(payload, indent=2))
        return

    for arm in ARMS:
        records = [run_one(arm) for _ in range(args.preflight)]
        payload["preflight"][arm] = records
        arm_spread = spread(records)
        payload.setdefault("preflight_spread", {})[arm] = arm_spread
        clean = arm_spread <= 0.02 and all(
            not record["timed_out"] and record["rc"] == 0 and record["oracle"] and
            record["before"]["load1"] < 20.0 and
            record["after"]["load1"] < 20.0 and
            record["before"]["foreign_cpu"] < 20.0 and
            record["after"]["foreign_cpu"] < 20.0 and
            record["wall_s"] < RUN_TIMEOUT_S for record in records)
        if not clean:
            payload["accepted"] = False
            payload["reason"] = f"A/A preflight rejected: {arm}"
            args.output.write_text(json.dumps(payload, indent=2))
            raise SystemExit(2)

    order = list(ARMS)
    for round_index in range(args.rounds):
        current = order if round_index % 2 == 0 else list(reversed(order))
        records = [run_one(arm) for arm in current]
        payload["rounds"].append(records)
        if any(record["timed_out"] or record["rc"] != 0 or not record["oracle"] or
               record["wall_s"] >= RUN_TIMEOUT_S or
               record["before"]["load1"] >= 20.0 or
               record["after"]["load1"] >= 20.0 or
               record["before"]["foreign_cpu"] >= 20.0 or
               record["after"]["foreign_cpu"] >= 20.0 for record in records):
            payload["accepted"] = False
            payload["reason"] = f"round {round_index + 1} contaminated"
            args.output.write_text(json.dumps(payload, indent=2))
            raise SystemExit(2)
    payload["accepted"] = True
    args.output.write_text(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
