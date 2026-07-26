#!/usr/bin/env bash
# run_helper_fault_tests.sh -- guest faults taken inside HOST helper frames.
#
#   run_helper_fault_tests.sh <path-to-svm_translator_linux>
#
# Sibling of run_malformed_guest_tests.sh, which asserts "the host survives".
# This one asserts the half above it: when the guest access that faults is
# performed by a host helper rather than by JIT code, the fault must STILL
# become a guest fault -- ExitReason::PageFatal -- and must not be quietly
# skipped or clamped.
#
# WHY THIS IS A SEPARATE SUITE.  A guest memory access normally happens in JIT
# code, where a host SIGSEGV/SIGBUS is caught by runtime.cpp's HandleFault,
# matched against the JIT fault table, and turned into PageFatal.  A handful of
# instructions are instead lowered to host helper calls that dereference guest
# memory directly -- x87's FNSTENV/FLDENV/FNSTCW/FLD m80, FXSAVE/FXRSTOR, and
# the whole rep movs/stos/cmps/scas family.  A fault inside those helpers
# happens in a live host frame whose pc is not in any JIT buffer, so
# HandleFault cannot recover it and the process used to die.  The workaround
# was to validate the address and, on failure, do nothing (x87/fxsave) or clamp
# the walk and move fewer bytes (rep).  That is the failure mode this project
# rates worst: a silently wrong answer.  What is asserted here is that the
# guest gets its #PF.
#
# ASSERTIONS, per case:
#   * the HOST must survive (no "unhandled host fault", no rc >= 128);
#   * the guest must halt with "reason 2" (ExitReason::PageFatal);
#   * the guest must NOT print SURVIVED -- every fault shape continues to a
#     marker write if the faulting instruction returns, so "the helper skipped
#     the access and execution went on" is caught explicitly rather than being
#     indistinguishable from a fault.
#
# clone_pf is the odd one out: there the fault is an ordinary JIT fault on a
# clone() worker, and what is under test is the *teardown* after it.  The
# worker's CLONE_CHILD_CLEARTID store is a host write into a guest page that
# is SMC write-protected (ctid shares a page with code), and it used to kill
# the host with SIGBUS on a thread with no active Runtime.  Assertion: the
# leader observes the ctid clear and exits 0 while the worker dies.
set -uo pipefail

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
GEN="$TESTS_DIR/gen_helper_fault_guest_x86_64.py"
SVM="${1:-}"
if [ -z "$SVM" ] || [ ! -x "$SVM" ]; then
    echo "usage: $0 <path-to-svm_translator_linux>" >&2
    exit 2
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
report() {  # report <name> <ok|bad> <detail>
    if [ "$2" = "ok" ]; then
        pass=$((pass + 1)); printf 'PASS  %-22s %s\n' "$1" "$3"
    else
        fail=$((fail + 1)); printf 'FAIL  %-22s %s\n' "$1" "$3"
    fi
}

run_shape() {  # run_shape <shape> [env...]
    local shape="$1"; shift
    local elf="$WORK/$shape.elf"
    python3 "$GEN" "$shape" -o "$elf" >/dev/null || { echo "generator failed"; return 9; }
    OUT="$(env "$@" "$SVM" "$elf" 2>&1)"
    RC=$?
}

# --- 1. clone worker page fault: guest thread dies, host lives -------------
echo "== a clone() worker's page fault must kill only that thread =="
for cfg in "default" "SVM_ENABLE_JIT=0"; do
    [ "$cfg" = "default" ] && envs=("SVM_UNUSED_PROBE=1") || envs=("$cfg")
    run_shape clone_pf "${envs[@]}"
    name="clone_pf[$cfg]"
    if echo "$OUT" | grep -q "unhandled host fault"; then
        report "$name" bad "HOST took an unhandled fault on the worker teardown path"
    elif [ "$RC" -ge 128 ]; then
        report "$name" bad "HOST killed by signal (rc=$RC)"
    elif [ "$RC" -eq 3 ]; then
        report "$name" bad "worker never cleared ctid: teardown did not run"
    elif [ "$RC" -ne 0 ]; then
        report "$name" bad "leader exited $RC (expected 0)"
    elif ! echo "$OUT" | grep -q "halted: reason 2"; then
        report "$name" bad "no worker PageFatal in the log -- the fault did not happen"
    else
        report "$name" ok "worker died of PageFatal, leader exited 0"
    fi
done

# --- 2. helper-resident faults must be guest faults ------------------------
echo "== an unmapped access made from a host helper must raise a guest #PF =="
X87_SHAPES="fnstenv fldenv fnstcw fld_m80 fxsave fxrstor"
REP_SHAPES="rep_movs rep_movs_partial rep_stos rep_stos_partial rep_scas rep_cmps"
# Three lowerings reach these helpers: the default SoftFloat/helper path, the
# x87 mid-tier (which bails to the same helper for the env/save forms), and the
# IR interpreter. All three must agree.
for cfg in "default" "SVM_X87_JIT=1" "SVM_ENABLE_JIT=0"; do
    [ "$cfg" = "default" ] && envs=("SVM_UNUSED_PROBE=1") || envs=("$cfg")
    for shape in $X87_SHAPES $REP_SHAPES; do
        run_shape "$shape" "${envs[@]}"
        name="$shape[$cfg]"
        if echo "$OUT" | grep -q "unhandled host fault"; then
            report "$name" bad "HOST took an unhandled fault"
        elif [ "$RC" -ge 128 ]; then
            report "$name" bad "HOST killed by signal (rc=$RC)"
        elif echo "$OUT" | grep -q "SURVIVED"; then
            report "$name" bad "the access was SKIPPED, not faulted: guest ran on"
        elif echo "$OUT" | grep -q "halted: reason 2"; then
            report "$name" ok "guest page fault taken (ExitReason::PageFatal)"
        else
            report "$name" bad "neither a fault nor a survival marker: $(echo "$OUT" | tail -1)"
        fi
    done
done

echo "----"
echo "helper-fault suite: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
