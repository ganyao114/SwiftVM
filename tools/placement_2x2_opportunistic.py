#!/usr/bin/env python3
"""Opportunistic placement 2x2: collect clean-window records across foreign-campaign gaps.

Every accepted record is measured in a verified-clean window (load<20 & foreign<20
before/during/after the run, oracle + rc checks). Contaminated records are discarded
and retried later — foreign load is independent of the arm's env vars, so discarding
does not bias the clean sample. Runs are scheduled in shuffled cycles of the 4 arms.
"""

import json
import os
import random
import re
import signal
import statistics
import subprocess
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSLATOR = ROOT / "build-master/source/translator/linux/svm_translator_linux"
COREMARK = Path("/Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64")
COMMAND = [str(TRANSLATOR), str(COREMARK), "0", "0", "0x66", "200000",
           "7", "1", "2000"]
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
    r"Python.+(?:test_|-m unittest|winemu-vcpu2|-c import os,sys,unittest)|"
    r"qemu-system-aarch64|/rustc(?: |$)|/clang(?:\+\+)?(?: |$)|codex.+vcpu2"
)
NEED_PER_ARM = 7
DEADLINE_S = 4 * 3600
RUN_TIMEOUT_S = 15.0
OUT = Path(os.environ.get("OPPORTUNISTIC_OUT",
                          "/Users/swift/.claude/jobs/c7ba5783/tmp/2x2-opportunistic.json"))


def foreign_cpu() -> float:
    result = subprocess.run(["ps", "-Ao", "pcpu=,command="], text=True,
                            stdout=subprocess.PIPE, check=True)
    total = 0.0
    for line in result.stdout.splitlines():
        fields = line.strip().split(maxsplit=1)
        if len(fields) == 2 and FOREIGN.search(fields[1]):
            total += float(fields[0])
    return total


def quiet_now() -> bool:
    return os.getloadavg()[0] < 8.0 and foreign_cpu() < 15.0


def run_one(arm: str, timeout_s: float = RUN_TIMEOUT_S) -> dict:
    env = os.environ.copy()
    env.pop("SVM_JIT_CACHE", None)
    env.pop("SVM_EXEC_PROF", None)
    env.update(ARMS[arm])
    before = {"load1": os.getloadavg()[0], "foreign_cpu": foreign_cpu()}
    during = []
    sample_errors = []
    stop_sampling = threading.Event()

    def sample_load() -> None:
        while not stop_sampling.wait(1.0):
            try:
                during.append({"load1": os.getloadavg()[0],
                               "foreign_cpu": foreign_cpu()})
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                sample_errors.append(str(error))
                return

    sampler = threading.Thread(target=sample_load)
    timed_out = False
    start = time.perf_counter()
    with subprocess.Popen(COMMAND, env=env, text=True, start_new_session=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as proc:
        sampler.start()
        try:
            try:
                out, _ = proc.communicate(timeout=timeout_s)
            except subprocess.TimeoutExpired:
                timed_out = True
                # Descendants can inherit stdout; killing only the parent
                # would leave communicate() waiting forever for their EOF.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                out, _ = proc.communicate()
        except BaseException:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
            raise
        finally:
            wall = time.perf_counter() - start
            stop_sampling.set()
            sampler.join()
    after = {"load1": os.getloadavg()[0], "foreign_cpu": foreign_cpu()}
    match = re.search(r"CoreMark 1\.0\s*:\s*([0-9.]+)", out)
    max_during_load = max((s["load1"] for s in during), default=0.0)
    max_during_foreign = max((s["foreign_cpu"] for s in during), default=0.0)
    clean = (not timed_out and not sample_errors and proc.returncode == 0
             and "crcfinal      : 0x4983" in out
             and "Correct operation validated" in out
             and before["load1"] < 20.0 and after["load1"] < 20.0
             and before["foreign_cpu"] < 20.0 and after["foreign_cpu"] < 20.0
             and max_during_load < 20.0 and max_during_foreign < 20.0
             and wall < 15.0)
    return {
        "arm": arm, "wall_s": wall, "rc": proc.returncode,
        "iter_s": float(match.group(1)) if match else None,
        "clean": clean, "before": before, "after": after,
        "timed_out": timed_out, "sample_errors": sample_errors,
        "max_during_load": max_during_load,
        "max_during_foreign": max_during_foreign,
        "ts": time.time(),
    }


def main() -> None:
    random.seed(20260811)
    start = time.monotonic()
    accepted: dict[str, list[dict]] = {arm: [] for arm in ARMS}
    discarded = []
    attempts = 0
    while time.monotonic() - start < DEADLINE_S:
        remaining = [a for a in ARMS if len(accepted[a]) < NEED_PER_ARM]
        if not remaining:
            break
        if not quiet_now():
            time.sleep(5)
            continue
        arm = random.choice(remaining)
        attempts += 1
        remaining_s = DEADLINE_S - (time.monotonic() - start)
        if remaining_s <= 0:
            break
        rec = run_one(arm, timeout_s=min(RUN_TIMEOUT_S, remaining_s))
        if rec["clean"]:
            accepted[arm].append(rec)
            print(f"[{time.monotonic()-start:7.0f}s] ACCEPT {arm} "
                  f"({len(accepted[arm])}/{NEED_PER_ARM}) wall={rec['wall_s']:.2f}",
                  flush=True)
        else:
            discarded.append(rec)
            print(f"[{time.monotonic()-start:7.0f}s] DISCARD {arm} "
                  f"wall={rec['wall_s']:.2f} rc={rec['rc']} "
                  f"before(f={rec['before']['foreign_cpu']:.0f}) "
                  f"during(f={rec['max_during_foreign']:.0f}) "
                  f"after(f={rec['after']['foreign_cpu']:.0f})", flush=True)
        time.sleep(2)

    done = all(len(v) == NEED_PER_ARM for v in accepted.values())
    payload = {"accepted": done, "attempts": attempts,
               "discarded": len(discarded),
               "elapsed_s": time.monotonic() - start,
               "records": {a: v for a, v in accepted.items()},
               "discarded_records": discarded}
    OUT.write_text(json.dumps(payload, indent=2))
    print(f"DONE accepted={done} attempts={attempts} discarded={len(discarded)}")
    for arm in ARMS:
        walls = [r["wall_s"] for r in accepted[arm]]
        if walls:
            print(f"  {arm}: n={len(walls)} median={statistics.median(walls):.3f} "
                  f"spread={(max(walls)-min(walls))/statistics.median(walls)*100:.2f}%")


if __name__ == "__main__":
    main()
