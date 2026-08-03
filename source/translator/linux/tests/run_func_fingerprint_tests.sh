#!/usr/bin/env bash
#
# Function-mode compile fingerprint gate.
#
# FIXED GUEST PATH -- read before touching the golden.
#   Dynamic-glibc guests put argv[0] on their initial stack.  Its string length
#   can change the reached blocks and function-unit split, so launching guests
#   directly from a checkout made the golden checkout-path dependent.  Every
#   guest, including static ones, is therefore staged below
#   /tmp/svm_fp_guests and launched through that fixed argv[0].  Keep this
#   staging when changing the corpus or regenerating the golden.
#
#   run_func_fingerprint_tests.sh <svm_translator_linux>            # vs golden
#   run_func_fingerprint_tests.sh <svm_translator_linux> --update   # rewrite golden
#   run_func_fingerprint_tests.sh <svm_A> --against <svm_B>         # A/B two builds
#
# PLATFORM / FEATURE GOLDEN SHARDS:
#   A function fingerprint is execution-driven. Host-visible syscall metadata
#   (for example /dev/null st_blksize) and lowering features can legitimately
#   select different guest units or IR. When a matching shard exists, check and
#   --update select:
#
#     func_fingerprint_golden.<darwin|linux>-<flagm|noflagm>.txt
#
#   `flagm` means CFINV is effective for this run: the host advertises FlagM and
#   SVM_FLAGS_CFINV is not forced to 0. SVM_FP_GOLDEN_PROFILE can name a fixture
#   explicitly, and SVM_FP_GOLDEN_DIR can redirect shards to a candidate
#   directory without touching the checked-in golden. If no shard has been
#   installed and neither override is set, the legacy single golden remains the
#   reference, preserving the historical command line and --update behaviour.
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
LEGACY_GOLDEN="$HERE/func_fingerprint_golden.txt"
STAGE_DIR="/tmp/svm_fp_guests"

detect_golden_profile() {
    local platform flagm=no
    case "$(uname -s)" in
        Darwin)
            platform=darwin
            if [ "${SVM_FLAGS_CFINV:-}" != 0 ] &&
               [ "$(sysctl -n hw.optional.arm.FEAT_FlagM 2>/dev/null || true)" = 1 ]; then
                flagm=yes
            fi
            ;;
        Linux)
            platform=linux
            if [ "${SVM_FLAGS_CFINV:-}" != 0 ] &&
               grep -qw flagm /proc/cpuinfo 2>/dev/null; then
                flagm=yes
            fi
            ;;
        *)
            # This translator is currently gated on Darwin/Linux, but keep an
            # unambiguous profile name for bring-up on another host.
            platform="$(uname -s | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9._-' '_')"
            ;;
    esac
    printf '%s-%s\n' "$platform" "$([ "$flagm" = yes ] && printf flagm || printf noflagm)"
}

GOLDEN_PROFILE="${SVM_FP_GOLDEN_PROFILE:-$(detect_golden_profile)}"
case "$GOLDEN_PROFILE" in
    *[!A-Za-z0-9._-]*|'')
        echo "FAIL: invalid SVM_FP_GOLDEN_PROFILE '$GOLDEN_PROFILE'"
        exit 2
        ;;
esac

GOLDEN_DIR="${SVM_FP_GOLDEN_DIR:-$HERE}"
SHARD_GOLDEN="$GOLDEN_DIR/func_fingerprint_golden.$GOLDEN_PROFILE.txt"
GOLDEN="$LEGACY_GOLDEN"
USING_GOLDEN_SHARD=no

if [ -n "${SVM_FP_GOLDEN_DIR:-}" ] || [ -n "${SVM_FP_GOLDEN_PROFILE:-}" ]; then
    # An explicit fixture request must never silently compare another profile.
    GOLDEN="$SHARD_GOLDEN"
    USING_GOLDEN_SHARD=yes
elif [ -f "$SHARD_GOLDEN" ]; then
    GOLDEN="$SHARD_GOLDEN"
    USING_GOLDEN_SHARD=yes
elif compgen -G "$HERE/func_fingerprint_golden.*.txt" >/dev/null; then
    # Once shards are installed, a new platform/feature combination requires
    # its own fixture instead of falling back to a misleading legacy profile.
    GOLDEN="$SHARD_GOLDEN"
    USING_GOLDEN_SHARD=yes
fi

if [ -z "$SVM" ] || [ ! -x "$SVM" ]; then
    echo "usage: run_func_fingerprint_tests.sh <svm_translator_linux> [--update|--against <svm_B>]"
    exit 2
fi

if [ "$USING_GOLDEN_SHARD" = yes ] && [ "$MODE" != --against ]; then
    echo "golden profile: $GOLDEN_PROFILE ($GOLDEN)"
fi

# Single-threaded, deterministic guests only. A multithreaded guest compiles a
# nondeterministic set of units (whichever thread reaches a region first), which
# would make this a flake generator rather than a gate.
GUESTS=(
    hello loop basic_coverage_smoke random_smoke vec_float_nan_pressure
    real_hello real_hello_musl real_busy real_busy_musl
    func_tests func_tests_musl
)

# Stage every guest behind a checkout-independent pathname. These MUST be
# copies, not symlinks: the loader realpath()s the guest path (loader.cpp) and
# pushes the RESOLVED path onto the guest stack as AT_EXECFN, so a symlink
# leaks the true checkout path into the guest's initial stack and the
# fingerprint becomes checkout-dependent again (this exact defeat was observed:
# __memcpy_ssse3 path selection shifted, ±4 units). A regular file at a fixed
# path realpaths to itself.
mkdir -p "$STAGE_DIR" || {
    echo "FAIL: cannot create fixed guest staging directory $STAGE_DIR"
    exit 2
}
for g in "${GUESTS[@]}"; do
    src="$HERE/${g}_x86_64"
    if [ -x "$src" ]; then
        # rm first: a stale SYMLINK here (from the pre-copy staging) would make
        # cp follow it and clobber the link target in another checkout.
        rm -f "$STAGE_DIR/${g}_x86_64"
        cp -f "$src" "$STAGE_DIR/${g}_x86_64" || {
            echo "FAIL: cannot stage $src at $STAGE_DIR"
            exit 2
        }
    fi
done
echo "guest staging: $STAGE_DIR (fixed argv[0]/AT_EXECFN, copies not symlinks)"

# One guest's fingerprint on stdout. `SVM_FUNC_BASE=1` is the default but is
# pinned here so the gate keeps meaning the same thing if the default moves.
# SVM_JIT_CACHE is normally cleared: a warm disk cache skips compilation
# entirely and would report an empty unit list as a pass. P3 coexistence tests
# may set SVM_FP_JIT_CACHE=1; all_fingerprints then gives each complete pass a
# distinct empty directory, so caching is enabled without turning the second
# determinism pass into a no-compilation warm run.
emit_fingerprint() {
    local svm="$1" guest="$2" keep_host="$3" cache_dir="$4"
    local source_bin="$HERE/${guest}_x86_64"
    local bin="$STAGE_DIR/${guest}_x86_64"
    [ -x "$source_bin" ] && [ -x "$bin" ] || { echo "$guest MISSING"; return; }
    local raw
    raw=$(SVM_PROF=2 SVM_FUNC_BASE=1 SVM_JIT_CACHE="$cache_dir" "$svm" "$bin" 2>&1 >/dev/null)
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
    local cache_dir=""
    if [ "${SVM_FP_JIT_CACHE:-0}" != 0 ]; then
        fp_cache_run=$((fp_cache_run + 1))
        cache_dir="$tmp/jit-cache-$fp_cache_run"
        mkdir -p "$cache_dir" || exit 2
    fi
    for g in "${GUESTS[@]}"; do
        emit_fingerprint "$svm" "$g" "$keep_host" "$cache_dir"
    done
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fp_cache_run=0

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
        mkdir -p "$(dirname "$GOLDEN")" || {
            echo "FAIL: cannot create golden directory $(dirname "$GOLDEN")"
            exit 2
        }
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
