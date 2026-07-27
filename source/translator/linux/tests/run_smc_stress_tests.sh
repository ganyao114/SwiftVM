#!/usr/bin/env bash
#
# SMC multithread stress regression.
#
#   run_smc_stress_tests.sh <svm_translator_linux> [runs] [per-run timeout s]
#
# Runs smc_mt_stress_x86_64: two guest threads execute four self-modifying
# worker units while two other guest threads rewrite those units' immediates,
# deliberately WITHOUT synchronisation. That interleaving reaches states the
# lock-stepped clone_smc_mt test cannot: a unit is published into the dispatch
# tables while another thread's write fault is already detaching it.
#
# The guest asserts nothing about the values it observes -- they race by
# design. The assertion is that the HOST survives and every guest thread
# finishes:
#     exit 0   all four workers reported in
#     exit 1   clone failed
#     exit 103 watchdog fired: only 3 of 4 workers reported (a guest thread
#              was halted by the runtime -- see "Guest thread N halted" on
#              stdout)
#     exit 134 host abort (this is what the SmcTracker use-after-free looked
#              like: PANIC "invalid address node type" in Module::DetachNode)
#     exit 132 host SIGILL (stale/reclaimed JIT code entered)
#
# Before the SmcTracker node-ownership fix this aborted on ~100% of runs.
#
# EVERY non-zero outcome below is a regression. Two defects this test used to
# report as "known open" are fixed (runtime.cpp, Runtime::Impl::Interpreter):
# under SMC churn the runtime fell into the IR interpreter for a dispatch slot
# whose module node was still live, and the interpreter cannot execute
# JIT-pipeline IR -- it yielded 0 for statically allocated uniforms (guest rsp
# -> PageFatal at `ret`, rc 103) and spun forever on ReturnToDispatch-class
# terminals (the timeout). Measured on the fix commit's parent: 14/550 runs
# lost a guest thread and 5/550 hung; after the fix 0/550 and 0/550.
#
# Sizing: those rates are ~2.5% and ~0.9% per run, so 200 runs detect a
# reintroduction with probability >99.9% (100 runs only ~97%). Keep the
# default at 200 unless you are iterating locally.
set -u
SVM="${1:?usage: run_smc_stress_tests.sh <svm_translator_linux> [runs] [timeout]}"
RUNS="${2:-200}"
TMO="${3:-60}"
HERE="$(cd "$(dirname "$0")" && pwd)"
GUEST="$HERE/smc_mt_stress_x86_64"

if [ ! -x "$GUEST" ]; then
    echo "MISSING $GUEST (regenerate with gen_smc_mt_stress_x86_64.py)"
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
host_fails=0      # host died: this is the regression this test gates on
guest_lost=0      # rc 103: a guest thread was halted (known open defect)
timeouts=0
: > "$tmp/sig"

for i in $(seq 1 "$RUNS"); do
    "$SVM" "$GUEST" >"$tmp/out" 2>"$tmp/err" &
    pid=$!
    ( sleep "$TMO"; kill -9 $pid 2>/dev/null ) &
    watchdog=$!
    wait $pid
    rc=$?
    kill $watchdog 2>/dev/null
    wait $watchdog 2>/dev/null
    case "$rc" in
        0)   ;;
        103) guest_lost=$((guest_lost + 1))
             grep -m1 'halted: reason' "$tmp/out" >> "$tmp/sig" 2>/dev/null ;;
        137) timeouts=$((timeouts + 1)); echo "TIMEOUT" >> "$tmp/sig" ;;
        *)   host_fails=$((host_fails + 1))
             echo "rc=$rc $(head -1 "$tmp/err")" >> "$tmp/sig" ;;
    esac
done

echo "smc_mt_stress: runs=$RUNS host_fails=$host_fails guest_lost=$guest_lost timeouts=$timeouts"
if [ "$host_fails" != 0 ] || [ "$guest_lost" != 0 ] || [ "$timeouts" != 0 ]; then
    echo "--- signatures ---"
    sort "$tmp/sig" | uniq -c | sort -rn | head -10
fi
# Both gates are hard. The exit codes stay distinct only so a failure says
# which class came back -- neither is tolerated.
#   exit 1  host abort/SIGILL: the SMC use-after-free class.
#   exit 2  a guest thread was halted, or a run hung: the runtime interpreted
#           JIT-pipeline IR on an SMC dispatch miss (see the header).
[ "$host_fails" = 0 ] || exit 1
[ "$guest_lost" = 0 ] && [ "$timeouts" = 0 ] || exit 2
exit 0
