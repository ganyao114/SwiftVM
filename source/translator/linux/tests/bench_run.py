#!/usr/bin/env python3
"""bench_run.py -- reproducible performance harness for SwiftVM guest workloads.

Usage:
    python3 source/translator/linux/tests/bench_run.py \
        --svm <build>/source/translator/linux/svm_translator_linux \
        [--reps 15] [--out bench_baseline.txt] [--config NAME=VAR=VAL,...]

What it does, and why each part exists
--------------------------------------
* Runs every workload REPS times and reports **median, min, and MAD** rather
  than a mean.  A mean is the wrong statistic on a shared laptop: one
  co-scheduled compile turns a 400 ms run into a 900 ms run and drags the mean
  with it, while the median ignores it.  `min` is reported as well because on a
  contended machine the minimum is the closest thing to an interference-free
  measurement, and median-vs-min tells you how contended the machine was.

* **Interleaves** configurations instead of running all reps of one back to
  back (round r of every (workload, config) pair, then round r+1).  Thermal
  drift and background load change over minutes; interleaving spreads that
  change across all configurations instead of concentrating it in whichever one
  happened to run while a build was going.

* Records the 1-minute **load average before and after** every rep and reports
  the range.  A baseline taken at load 8 is not comparable with one taken at
  load 0.5, and the only defence is to write the load down.

* Discards the first rep of each (workload, config) as a warm-up (page cache,
  dyld, the guest ELF's first read from disk).

* **Verifies the checksum** of every rep against the golden file.  A
  performance number from a run that computed the wrong answer is worthless,
  and a mismatch aborts rather than being reported.

* Uses CLOCK_MONOTONIC around fork/exec, and also records the child's user+sys
  CPU time from wait4.  CPU time is less sensitive to co-scheduling than wall
  clock (though still sensitive to DVFS), so a large wall/cpu divergence is
  itself evidence of interference.

Machine noise on Apple Silicon
------------------------------
This harness cannot pin frequency: macOS exposes no governor, and the P/E core
split means a descheduled-then-rescheduled process can resume on an E core at a
fraction of the clock.  There is no `taskset`.  The mitigations available are
exactly the ones above -- many reps, median + min, interleaving, and writing
down the load -- plus running with the machine otherwise idle.  Do not compare
numbers across runs whose recorded load ranges do not overlap.
"""

import argparse
import json
import os
import re
import resource
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# (name, guest binary, argv, expected-stdout-key, expected exit code, description)
# Every entry must run long enough that the ~15 ms translator startup floor is
# a small fraction of the total; see bench_suite_kernels.h for the calibration.
WORKLOADS = [
    ("int",    "bench_suite_x86_64", ["int", "1"],    "int",    0, "64-bit integer ALU, dependency chains, no memory"),
    ("fp",     "bench_suite_x86_64", ["fp", "1"],     "fp",     0, "SSE2 packed double/single arithmetic"),
    ("mem",    "bench_suite_x86_64", ["mem", "1"],    "mem",    0, "4 MiB streaming + strided + pointer chase"),
    ("branch", "bench_suite_x86_64", ["branch", "1"], "branch", 0, "unpredictable data-dependent branches"),
    ("call",   "bench_suite_x86_64", ["call", "1"],   "call",   0, "4-deep non-inlined guest call chain"),
    # Real programs, kept for translation-cost coverage: these are dominated by
    # one-shot translation of a large static glibc rather than by execution.
    # func_tests exit code 101 and the x87 bench exit 0 are their normal results;
    # checking them is not pedantry -- SVM_FUNC_BASE=0 aborts the x87 bench with
    # "No free temporary GPR", and without an exit-code check that abort is
    # silently reported as an 11x speedup.
    ("func_tests", "func_tests_x86_64", [], None, 101, "real glibc program, 7 kernels (translation-heavy)"),
    ("x87_bench",  "x87_bench_x86_64",  [], None, 0, "x87 throughput (SoftFloat helper path by default)"),
]

GOLDEN = os.path.join(HERE, "bench_suite_x86_64.native.txt")


def require_inputs(workloads):
    """Abort unless every guest and every golden checksum this run needs exists.

    This is not defensive padding.  The guest ELFs live under a `*_x86_64`
    .gitignore pattern and are force-added one by one; bench_suite_x86_64 and
    bench_suite_x86_64.native.txt were never added, so in every fresh clone or
    worktree this harness skipped 5 of its 7 workloads and compared 0 checksums
    -- and still printed a well-formed baseline table.  A harness that reports
    success on 2/7 coverage is worse than one that does not run: it launders a
    coverage hole into a green result.  Missing input is a hard error, checked
    up front so it costs a second rather than a 15-rep run.
    """
    missing = []
    for name, guest, _argv, key, _rc, _desc in workloads:
        gpath = os.path.join(HERE, guest)
        if not os.path.exists(gpath):
            missing.append("workload %-10s guest missing: %s" % (name, gpath))
    if not os.path.exists(GOLDEN):
        missing.append("golden checksum table missing: %s" % GOLDEN)
    else:
        golden = load_golden()
        for name, _guest, _argv, key, _rc, _desc in workloads:
            if key is not None and key not in golden:
                missing.append("workload %-10s has no '%s' entry in %s"
                               % (name, key, os.path.basename(GOLDEN)))
    if missing:
        sys.exit("FATAL: benchmark inputs are missing -- refusing to report a\n"
                 "partial run as a baseline:\n  %s\n\n"
                 "Rebuild them with:\n"
                 "  bash %s/build_bench_tests.sh\n"
                 "and make sure they are tracked by git (see the exceptions at the\n"
                 "bottom of .gitignore) so a fresh clone measures the same things."
                 % ("\n  ".join(missing), HERE))


def load_golden():
    out = {}
    if not os.path.exists(GOLDEN):
        return out
    for line in open(GOLDEN):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("exit="):
            continue
        parts = line.split()
        if len(parts) == 2:
            out[parts[0]] = parts[1]
    return out


def loadavg():
    return os.getloadavg()[0]


def run_once(svm, guest, argv, env):
    """Return (wall_seconds, cpu_seconds, stdout, exit_code)."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.monotonic()
    p = subprocess.run([svm, guest] + argv, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, env=env)
    wall = time.monotonic() - t0
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = ((after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime))
    return wall, cpu, p.stdout.decode("utf-8", "replace"), p.returncode


def mad(xs, med):
    return statistics.median([abs(x - med) for x in xs]) if xs else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--svm", required=True, help="path to svm_translator_linux")
    ap.add_argument("--reps", type=int, default=15)
    ap.add_argument("--out", default=None, help="write results here (default stdout only)")
    ap.add_argument("--config", action="append", default=[],
                    help="NAME=VAR=VAL[:VAR=VAL...]; repeatable. Default: a single "
                         "'default' config with no extra environment.")
    ap.add_argument("--only", default=None, help="comma-separated workload names")
    args = ap.parse_args()

    configs = [("default", {})]
    if args.config:
        configs = []
        for spec in args.config:
            name, _, rest = spec.partition("=")
            envd = {}
            if rest:
                for kv in rest.split(":"):
                    k, _, v = kv.partition("=")
                    envd[k] = v
            configs.append((name, envd))

    only = set(args.only.split(",")) if args.only else None
    workloads = [w for w in WORKLOADS if only is None or w[0] in only]
    if only:
        unknown = only - {w[0] for w in WORKLOADS}
        if unknown:
            sys.exit("FATAL: --only names no such workload: %s (have: %s)"
                     % (", ".join(sorted(unknown)), ", ".join(w[0] for w in WORKLOADS)))
    require_inputs(workloads)

    golden = load_golden()
    base_env = dict(os.environ)
    base_env.pop("SVM_PROF", None)

    samples = {}   # (workload, config) -> {"wall": [...], "cpu": [...]}
    for w in workloads:
        for c, _ in configs:
            samples[(w[0], c)] = {"wall": [], "cpu": []}

    load_lo, load_hi = 1e9, -1e9
    t_start = time.time()

    # rep 0 is a discarded warm-up; interleave everything else.
    for rep in range(args.reps + 1):
        for name, guest, argv, key, want_rc, _desc in workloads:
            gpath = os.path.join(HERE, guest)   # existence proven by require_inputs()
            for cname, cenv in configs:
                env = dict(base_env)
                env.update(cenv)
                la = loadavg()
                load_lo, load_hi = min(load_lo, la), max(load_hi, la)
                wall, cpu, out, rc = run_once(args.svm, gpath, argv, env)
                if want_rc is not None and rc != want_rc:
                    sys.exit("FATAL: %s/%s exited %d, expected %d -- a crashed run is "
                             "not a fast run\n%s" % (name, cname, rc, want_rc, out[-800:]))
                if key is not None:   # golden[key] proven present by require_inputs()
                    m = re.search(r"^%s\s+(\S+)$" % re.escape(key), out, re.M)
                    if not m:
                        sys.exit("FATAL: %s/%s produced no '%s' line (rc=%d)\n%s"
                                 % (name, cname, key, rc, out[-800:]))
                    if m.group(1) != golden[key]:
                        sys.exit("FATAL: checksum mismatch %s/%s: got %s want %s"
                                 % (name, cname, m.group(1), golden[key]))
                if rep > 0:
                    samples[(name, cname)]["wall"].append(wall)
                    samples[(name, cname)]["cpu"].append(cpu)
        print("  rep %d/%d done (%.0fs elapsed, load %.2f)"
              % (rep, args.reps, time.time() - t_start, loadavg()), file=sys.stderr)

    lines = []
    lines.append("# SwiftVM performance baseline")
    lines.append("# translator: %s" % args.svm)
    lines.append("# reps=%d (plus 1 discarded warm-up), configs interleaved" % args.reps)
    lines.append("# host 1-min loadavg during the run: %.2f .. %.2f" % (load_lo, load_hi))
    lines.append("# build: see the accompanying docs/perf-baseline.md for build type")
    lines.append("# columns: workload config median_s min_s mad_s cpu_median_s rel_mad")
    for name, guest, argv, key, want_rc, desc in workloads:
        for cname, _ in configs:
            s = samples.get((name, cname))
            if not s or not s["wall"]:
                continue
            med = statistics.median(s["wall"])
            lines.append("%-11s %-14s %8.4f %8.4f %8.4f %8.4f %6.2f%%"
                         % (name, cname, med, min(s["wall"]), mad(s["wall"], med),
                            statistics.median(s["cpu"]), 100.0 * mad(s["wall"], med) / med))
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if args.out:
        open(args.out, "w").write(text)
        raw = os.path.splitext(args.out)[0] + ".raw.json"
        json.dump({("%s/%s" % k): v for k, v in samples.items()}, open(raw, "w"), indent=1)


if __name__ == "__main__":
    main()
