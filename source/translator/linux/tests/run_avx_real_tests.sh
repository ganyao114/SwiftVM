#!/usr/bin/env bash
# run_avx_real_tests.sh -- end-to-end qualification of SwiftVM's AVX support
# against REAL PROGRAMS, not encoding probes.
#
#   run_avx_real_tests.sh <path-to-svm_translator_linux>
#
# Three questions, in order of what they are worth:
#
#  1. Does a real -mavx2 -O2 program compute the same bits under SwiftVM as it
#     does on an x86-64 CPU?  Each of the 11 kernels in avx_real_x86_64 runs on
#     its own and is compared, bit for bit, against the golden line recorded
#     from an actual x86-64 execution of the same kernels
#     (avx_real_x86_64.native.txt -- regenerate with build_avx_real_tests.sh).
#     Per-kernel isolation is deliberate: an unimplemented instruction KILLS
#     the guest, so a single whole-program run would hide every kernel after
#     the first gap.
#
#  2. Does the SVM_AVX gate actually gate?  The same binary is run with
#     SVM_AVX unset; it must die, because with the gate off every VEX opcode
#     falls through to FALLBACK -> ExitReason::IllegalCode.  A guest that
#     SURVIVES that run would mean AVX is reachable without the opt-in.
#
#  3. Does a 32-byte access that straddles a page boundary behave?  See
#     avx_crosspage_common.h.  Stage 0 (both pages mapped) is compared against
#     the oracle; stages 1-4 must produce a guest page fault and not a silent
#     success; stage 5 wants to measure whether a faulting straddling store
#     leaves a torn write behind, and is currently BLOCKED (see below).
#
# KNOWN_GAPS below is the honest, quantified list of what is missing today.  A
# kernel in that list is reported as EXPECTED-GAP and does not fail the run; a
# kernel that dies WITHOUT being listed, or that produces different bits, is a
# hard failure.  Shrinking KNOWN_GAPS is the point of the exercise.
set -uo pipefail

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
SVM="${1:-}"
if [ -z "$SVM" ] || [ ! -x "$SVM" ]; then
    echo "usage: $0 <path-to-svm_translator_linux>" >&2
    exit 2
fi

GUEST="$TESTS_DIR/avx_real_x86_64"
GOLD="$TESTS_DIR/avx_real_x86_64.native.txt"
XGUEST="$TESTS_DIR/avx_crosspage_x86_64"
XGOLD="$TESTS_DIR/avx_crosspage_x86_64.native.txt"
NAMES=(fdot fedge dmath matmul iproc ishift xlane memops vzu sysst gather)

# Kernels whose instruction stream contains an opcode the frontend cannot
# translate yet.  Each entry names the instruction the guest actually died on,
# found by disassembling around the rip in the "halted: reason 1" message.
declare -a KNOWN_GAPS=(
    "0:fdot:vhaddps ymm (VEX.256.F2.0F 7C); also vextractps (VEX.128.66.0F3A 17)"
    "3:matmul:vblendps (VEX.256.66.0F3A 0C); also vmaskmovps (VEX.256.66.0F38 2E)"
    "4:iproc:vpmaddubsw (VEX.256.66.0F38 04); also vpmuludq F4, vpmuldq 28, vphaddw 01, vphsubd 06"
    "10:gather:vpgatherdd/vgatherdps (VEX.256.66.0F38 90/92, VSIB)"
)

fail=0
pass=0
gap=0

gold_line() {  # gold_line <section> <key>
    awk -v sec="[$1]" -v key="$2=" '
        $0 == sec { inside = 1; next }
        /^\[/     { inside = 0 }
        inside && index($0, key) == 1 { print; exit }' "$3"
}

known_gap_for() {
    local idx="$1"
    for entry in "${KNOWN_GAPS[@]}"; do
        [ "${entry%%:*}" = "$idx" ] && { echo "${entry#*:*:}"; return 0; }
    done
    return 1
}

echo "=== 1. real AVX2 program vs x86-64 hardware, kernel by kernel ==="
for i in "${!NAMES[@]}"; do
    name="${NAMES[$i]}"
    want="$(gold_line "$i" "$name" "$GOLD")"
    # One run, both streams: the result line and the "halted" diagnostic do not
    # go to the same stream, and the rip in that diagnostic is what names the
    # instruction the guest died on.
    out="$(SVM_AVX=1 "$SVM" "$GUEST" "$i" 2>&1)"
    got="$(echo "$out" | grep "^$name=" | head -1)"
    if [ -n "$got" ] && [ "$got" = "$want" ]; then
        echo "  PASS        K$i $name  $got"
        pass=$((pass + 1))
    elif [ -n "$got" ]; then
        echo "  FAIL DIFF   K$i $name  svm=$got  x86=$want"
        fail=$((fail + 1))
    else
        rip="$(echo "$out" | grep -oE 'rip = 0x[0-9a-f]+' | head -1)"
        if why="$(known_gap_for "$i")"; then
            echo "  EXPECTED-GAP K$i $name  guest killed at $rip -- $why"
            gap=$((gap + 1))
        else
            echo "  FAIL FATAL  K$i $name  guest killed at $rip (NOT in KNOWN_GAPS)"
            fail=$((fail + 1))
        fi
    fi
done

echo "=== 2. SVM_AVX gate ==="
# Unset gate: the very first VEX instruction must be fatal.  Checked on kernel
# 1 (fedge), which passes cleanly when the gate is on -- so a survival here
# could only mean the gate leaks.
out="$("$SVM" "$GUEST" 1 2>&1)"
if echo "$out" | grep -q "halted: reason 1"; then
    echo "  PASS        SVM_AVX unset -> guest killed with IllegalCode as designed"
    pass=$((pass + 1))
elif echo "$out" | grep -q "^fedge="; then
    echo "  FAIL        SVM_AVX unset but the AVX kernel still ran: the gate leaks"
    fail=$((fail + 1))
else
    echo "  FAIL        SVM_AVX unset -> unexpected outcome:"
    echo "$out" | sed 's/^/                /'
    fail=$((fail + 1))
fi

echo "=== 3. 32-byte access across a page boundary ==="
want="$(gold_line 0 straddle_mapped "$XGOLD")"
got="$(SVM_AVX=1 "$SVM" "$XGUEST" 0 2>/dev/null | grep '^straddle_mapped=')"
if [ -n "$got" ] && [ "$got" = "$want" ]; then
    echo "  PASS        stage 0 both pages mapped: $got"
    pass=$((pass + 1))
else
    echo "  FAIL        stage 0  svm=${got:-<none>}  x86=$want"
    fail=$((fail + 1))
fi
for s in 1 2 3 4; do
    out="$(SVM_AVX=1 "$SVM" "$XGUEST" "$s" 2>&1)"
    if echo "$out" | grep -q "survived_no_fault"; then
        echo "  FAIL        stage $s: the access into the unmapped page did NOT fault"
        fail=$((fail + 1))
    elif echo "$out" | grep -q "halted: reason 2"; then
        echo "  PASS        stage $s: guest page fault taken (ExitReason::PageFatal)"
        pass=$((pass + 1))
    else
        echo "  FAIL        stage $s: neither a fault nor a clean run:"
        echo "$out" | grep -v 'fixed map failed\|stack placed' | sed 's/^/                /'
        fail=$((fail + 1))
    fi
done
# Stage 5 is the measurement we actually want (does the split lowering leave a
# torn write?).  It needs an observer that outlives the fault; SwiftVM has no
# guest signal delivery, so it uses a clone() worker -- and a worker that dies
# of PageFatal currently brings the host process down with it, which is a
# separate bug.  Report the state of that blocker instead of pretending to
# have an answer.
out="$(SVM_AVX=1 "$SVM" "$XGUEST" 5 2>&1)"
if echo "$out" | grep -q "^first_page_partial="; then
    echo "  INFO        stage 5 measured: $(echo "$out" | grep '^first_page_partial=')"
    echo "              (0 = hardware-like, 1 = torn write from the 2x16B lowering)"
elif echo "$out" | grep -q "unhandled host fault"; then
    echo "  BLOCKED     stage 5: a clone() worker dying of PageFatal takes the host"
    echo "              process down (unhandled host SIGBUS), so nothing survives to"
    echo "              read the first page back.  Torn-write question UNANSWERED."
else
    echo "  BLOCKED     stage 5: unexpected outcome"
    echo "$out" | grep -v 'fixed map failed\|stack placed' | sed 's/^/                /'
fi

echo "=== summary: $pass passed, $gap expected gaps, $fail failed ==="
[ "$fail" -eq 0 ]
