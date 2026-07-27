#!/usr/bin/env bash
#
# Function-mode compile fingerprint gate.
#
# PATH SENSITIVITY -- read before touching the golden.
#   real_hello / real_busy are dynamically-linked glibc guests whose initial
#   stack contains argv[0].  A different path STRING LENGTH shifts that stack,
#   which shifts which blocks the guest reaches, which changes the unit list.
#   Measured: 4428 units from the canonical repo path, 4447 from a git
#   worktree under /private/tmp -- same commit, same binary content.
#
#   So: ALWAYS regenerate and compare from the canonical repo checkout, never
#   from a worktree.  The script's own determinism check cannot see this
#   because it reruns from the same path.  This bit the main line once: a
#   golden regenerated from a worktree looked self-consistent and made the
#   gate red for everyone else.
#
#   run_func_fingerprint_tests.sh <svm_translator_linux>            # vs golden
#   run_func_fingerprint_tests.sh <svm_translator_linux> --update   # rewrite golden
#   run_func_fingerprint_tests.sh <svm_A> --against <svm_B>         # A/B two builds
#
# WHY THIS EXISTS: GUEST EXIT CODES ARE BLIND TO FUNCTION-MODE MISCOMPILES.
#
# The function compiler is wrapped in try/catch. A unit that miscompiles badly
# enough to trip an assertion does not fail the run -- the exception unwinds,
# the driver falls back to compiling that guest region as individual blocks,
# and the guest goes on to produce its correct exit code. The 25-binary e2e
# matrix therefore stays green while the thing it is supposed to be exercising
# has silently stopped running. This was not hypothetical: a deliberate
# mutation of the function path (M1) was first scored as SURVIVED on exactly
# this evidence, and only the per-unit emission trace showed it had in fact
# knocked every function unit out to the block fallback.
#
# What is compared, and why only this:
#   per-unit (guest pc, ir count)   cross-build stable, and the direct witness
#                                   of "this guest region compiled as a
#                                   function unit, with this much IR"
#   func_units / block_units /      cross-build stable totals; a fallback moves
#   decoded_blocks / ir_insts       counts from func_units to block_units
#   host_bytes                      NOT compared across builds. The emitted
#                                   host pointer immediates change length with
#                                   the translator's own layout, so this drifts
#                                   by hundreds of bytes between builds that
#                                   generate identical code. It IS compared
#                                   run-to-run within one build (below), where
#                                   a difference means the emitter is
#                                   nondeterministic.
#
# Exit: 0 all gates pass, 1 a fingerprint differs, 2 harness/setup problem.
set -u

SVM="${1:-}"
MODE="${2:-check}"
OTHER="${3:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
GOLDEN="$HERE/func_fingerprint_golden.txt"

if [ -z "$SVM" ] || [ ! -x "$SVM" ]; then
    echo "usage: run_func_fingerprint_tests.sh <svm_translator_linux> [--update|--against <svm_B>]"
    exit 2
fi

# Single-threaded, deterministic guests only. A multithreaded guest compiles a
# nondeterministic set of units (whichever thread reaches a region first), which
# would make this a flake generator rather than a gate.
GUESTS=(
    hello loop basic_coverage_smoke random_smoke vec_float_nan_pressure
    real_hello real_hello_musl real_busy real_busy_musl
    func_tests func_tests_musl
)

# One guest's fingerprint on stdout. `SVM_FUNC_BASE=1` is the default but is
# pinned here so the gate keeps meaning the same thing if the default moves.
# SVM_JIT_CACHE is cleared: a warm disk cache skips compilation entirely and
# would report an empty unit list as a pass.
emit_fingerprint() {
    local svm="$1" guest="$2" keep_host="$3"
    local bin="$HERE/${guest}_x86_64"
    [ -x "$bin" ] || { echo "$guest MISSING"; return; }
    local raw
    raw=$(SVM_PROF=2 SVM_FUNC_BASE=1 SVM_JIT_CACHE= "$svm" "$bin" 2>&1 >/dev/null)
    local units
    if [ "$keep_host" = yes ]; then
        units=$(printf '%s\n' "$raw" | sed -n 's/^\[svm-unit\] //p' | sort)
    else
        units=$(printf '%s\n' "$raw" | sed -n 's/^\[svm-unit\] //p' | sed 's/ host=[0-9]*$//' | sort)
    fi
    printf '%s\n' "$units" | sed "s/^/$guest /"
    # Totals line: host_bytes and the *_ns timings are deliberately dropped.
    printf '%s\n' "$raw" \
        | sed -n 's/^\[svm-prof\] \(func_units=.*\)$/\1/p' \
        | sed 's/ host_bytes=[0-9]*//; s/ pool_bytes=[0-9]*//' \
        | sed "s/^/$guest TOTALS /"
}

all_fingerprints() {
    local svm="$1" keep_host="$2"
    for g in "${GUESTS[@]}"; do emit_fingerprint "$svm" "$g" "$keep_host"; done
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- gate 1: the emitter is deterministic within this build -----------------
# host_bytes is kept here on purpose; this is the one comparison it is valid
# for. Two runs of the SAME binary must emit byte-identical unit lists.
all_fingerprints "$SVM" yes > "$tmp/self_a"
all_fingerprints "$SVM" yes > "$tmp/self_b"
if ! diff -u "$tmp/self_a" "$tmp/self_b" > "$tmp/self_diff"; then
    echo "FAIL: this build is not deterministic -- two runs disagree"
    head -40 "$tmp/self_diff"
    exit 1
fi
units_total=$(grep -c ' pc=' "$tmp/self_a" || true)
echo "self-consistency: OK ($units_total function units over ${#GUESTS[@]} guests, host_bytes included)"

if grep -q MISSING "$tmp/self_a"; then
    echo "FAIL: guest binaries missing:"
    grep MISSING "$tmp/self_a" | sort -u
    exit 2
fi

# A build that compiled nothing as a function would pass a list-vs-list diff
# against a golden generated from the same broken build. Refuse that outright.
if [ "$units_total" -lt 1000 ]; then
    echo "FAIL: only $units_total function units emitted -- the function path is"
    echo "      not running (expect >3000). This is exactly the failure guest"
    echo "      exit codes cannot see."
    exit 1
fi

all_fingerprints "$SVM" no > "$tmp/a"

# --- gate 2: cross-build comparison ----------------------------------------
case "$MODE" in
    --update)
        cp "$tmp/a" "$GOLDEN"
        echo "wrote $GOLDEN ($(grep -c ' pc=' "$GOLDEN") units)"
        exit 0
        ;;
    --against)
        if [ -z "$OTHER" ] || [ ! -x "$OTHER" ]; then
            echo "usage: run_func_fingerprint_tests.sh <svm_A> --against <svm_B>"
            exit 2
        fi
        all_fingerprints "$OTHER" no > "$tmp/b"
        REF="$tmp/b"; REFNAME="$OTHER"
        ;;
    check)
        if [ ! -f "$GOLDEN" ]; then
            echo "FAIL: no golden at $GOLDEN (regenerate with --update)"
            exit 2
        fi
        REF="$GOLDEN"; REFNAME="$GOLDEN"
        ;;
    *)
        echo "unknown mode: $MODE"
        exit 2
        ;;
esac

if diff -u "$REF" "$tmp/a" > "$tmp/diff"; then
    echo "fingerprint: OK (matches $REFNAME)"
    exit 0
fi

echo "FAIL: function-mode emission fingerprint differs from $REFNAME"
echo "--- per-guest unit count and IR total ---"
for g in "${GUESTS[@]}"; do
    ra=$(grep -c "^$g pc=" "$REF" || true)
    rb=$(grep -c "^$g pc=" "$tmp/a" || true)
    ta=$(grep "^$g TOTALS" "$REF" || true)
    tb=$(grep "^$g TOTALS" "$tmp/a" || true)
    if [ "$ra" != "$rb" ] || [ "$ta" != "$tb" ]; then
        echo "  $g: units $ra -> $rb"
        echo "      ref  ${ta#"$g TOTALS "}"
        echo "      this ${tb#"$g TOTALS "}"
    fi
done
echo "--- first 40 differing unit lines ---"
head -40 "$tmp/diff"
exit 1
