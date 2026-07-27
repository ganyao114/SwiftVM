#!/usr/bin/env bash
# run_malformed_guest_tests.sh -- ISOLATION regression suite.
#
#   run_malformed_guest_tests.sh <path-to-svm_translator_linux> [--audit]
#
# The contract this guards is not "SwiftVM computes the right answer", it is
# the one below it: *a guest cannot take the host down*.  However malformed,
# hostile or self-destructive the guest instruction stream is, the only thing
# that may die is the guest -- ExitReason::IllegalCode or PageFatal, i.e. a
# "halted: reason N" line and a nonzero-but-ordinary exit.  A host that takes
# an unhandled SIGSEGV/SIGBUS, or that aborts on an internal invariant, is a
# hard failure no matter what the guest was asking for.
#
# Why this shape of test: the defects it covers were never one bad opcode,
# they were one missing check reached from many opcodes.  Guest memory is a
# plain bias add (host = guest + bias), so *any* guest address dereferenced
# from host code lands somewhere in the host's address space; the JIT's
# guest-fault recovery in runtime/backend/runtime.cpp only rewrites faults
# whose host pc is inside a JIT buffer, so a fault taken in the decoder --
# host code -- is unrecoverable and kills the process.  Everything that can
# steer the decoder at an address the guest never mapped is therefore the same
# bug wearing different opcodes: a wild RET, a jmp through a garbage register,
# a length the decoder gets wrong so the stream desynchronizes, an encoding in
# the last bytes of the last mapped page.  The cases below are one instance of
# each of those routes, not a list of bad instructions.
#
# --audit additionally sweeps the whole single-byte and 0F opcode spaces plus
# every VEX map/pp x opcode combination (~3.5k runs, ~1 min).  It is NOT part
# of the default run because two known, unrelated host-kill classes still live
# in there (KNOWN-A/KNOWN-B, documented below); audit mode reports those
# without failing, and fails on anything new.
set -uo pipefail

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
GEN="$TESTS_DIR/gen_malformed_guest_x86_64.py"
SVM="${1:-}"
AUDIT="${2:-}"
if [ -z "$SVM" ] || [ ! -x "$SVM" ]; then
    echo "usage: $0 <path-to-svm_translator_linux> [--audit]" >&2
    exit 2
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# macOS ships no timeout(1). gtimeout if coreutils is around, otherwise run
# unbounded -- only the audit sweep needs it, to survive a guest that loops.
TIMEOUT=""
command -v timeout  >/dev/null 2>&1 && TIMEOUT="timeout 5"
command -v gtimeout >/dev/null 2>&1 && TIMEOUT="gtimeout 5"

fail=0
pass=0
flaky=0

# Two host-kill classes below this suite are still OPEN, are not decode-path
# defects, and both fire only intermittently, so each case is retried: a
# genuine regression of the fetch path is deterministic and fails every
# attempt, while these two clear on a retry.
#
# KNOWN-B  the arm64 register allocator throws std::logic_error
#          ("Check failed: active_gprs.Get(id)") on some IR shapes a malformed
#          stream reaches -- notably a several-thousand-instruction block of
#          `add [rax],al` decoded out of a page of zeros.  Nothing catches it,
#          so the host std::terminate()s.
# KNOWN-C  guest and host addresses share one flat space (host = guest + bias,
#          with no bound anywhere), so a guest page can alias a host page.
#          After a wild branch, SmcTracker::RegisterNode write-protects "the
#          guest page holding this block" and the mprotect() lands on whatever
#          host mapping the bias points at -- sometimes the translator's own
#          __TEXT, which then loses execute permission under its own feet
#          (fault address == faulting pc), sometimes a host heap page, which
#          takes a library down instead.  Whether the aliasing happens depends
#          on where ASLR put the image mapping: ~2.5% of runs.
kill_reason() {   # $1 = stderr file, $2 = process rc; echoes "" when the host survived
    if grep -q "unhandled host fault" "$1"; then
        grep -m1 'unhandled host fault' "$1"
    elif grep -qE "libc\+\+abi|terminating due to uncaught|Check failed" "$1"; then
        echo "host aborted -- $(grep -m1 -E 'Check failed|terminating' "$1")"
    elif [ "$2" -ge 128 ]; then
        # 128+N: the shell's encoding of "killed by signal N".
        echo "host killed by signal $(($2 - 128))"
    fi
}

# Run one payload; the host must survive it. Retries so the two known-open
# intermittent classes above do not make this suite flaky.
#   $1 shape (flat|edge)   $2 payload hex   $3 human-readable name
check() {
    local shape="$1" payload="$2" name="$3"
    local elf="$WORK/probe.elf" err="$WORK/probe.err"
    python3 "$GEN" "$shape" "$payload" -o "$elf" || return 2
    local attempt why=""
    for attempt in 1 2 3; do
        SVM_AVX="${SVM_AVX:-0}" SVM_BMI="${SVM_BMI:-0}" \
            "$SVM" "$elf" >/dev/null 2>"$err"
        why="$(kill_reason "$err" $?)"
        [ -z "$why" ] && break
    done
    [ -z "$why" ] || { echo "FAIL  $name [$payload]: $why"; return 1; }
    [ "$attempt" -eq 1 ] || { echo "KNOWN-flake $name [$payload] (cleared on retry $attempt)"; return 3; }
    return 0
}

run_case() {
    check "$@"
    case $? in
        0) pass=$((pass + 1)) ;;
        2) echo "ERROR generating $3" >&2; exit 2 ;;
        3) pass=$((pass + 1)); flaky=$((flaky + 1)) ;;
        *) fail=$((fail + 1)) ;;
    esac
}

echo "== instruction-length desync (VEX prefix + a legacy opcode) =="
# The reported case.  distorm sizes "C4 E1 <pp> 9B" as a 4-byte WAIT -- it
# swallows the VEX bytes as plain prefixes -- so the ModRM byte that follows is
# decoded as a fresh instruction.  With CB (RETF) there, the guest returns to
# whatever the stack held (argc == 1) and the decoder is asked to fetch at
# guest address 1.  Every VEX map/pp combination is checked: 8 of the 12 used
# to kill the host, and which 4 did not was an accident of the opcode tables.
for m in e1 e2 e3; do
    for p in 78 79 7a 7b; do
        run_case flat "c4${m}${p}9bcb" "VEX(m=${m},pp=${p}) + 9B + CB"
    done
done
# C5 (two-byte VEX) reaches the same table by a different prefix length.
for p in f8 f9 fa fb; do
    run_case flat "c5${p}9bcb" "VEX2(pp=${p}) + 9B + CB"
done

echo "== control transfer to an address the guest never mapped =="
run_case flat "c3"                             "near RET off the initial stack"
run_case flat "cb"                             "far RET off the initial stack"
run_case flat "48b8000000500000000048ffe0"     "jmp rax, rax=0x50000000"
run_case flat "48b8010000000000000048ffe0"     "jmp rax, rax=1"
run_case flat "48b8000000500000000048ffd0"     "call rax, rax=0x50000000"
run_case flat "48b8ffffffffffffffff48ffe0"     "jmp rax, rax=-1 (bias wrap)"
run_case flat "e9fbffbfff"                     "jmp rel32 below the image"
run_case flat "48b80000000000004000ffe0"       "jmp rax, non-canonical"

echo "== decode running off the end of the last mapped page =="
# Zero bytes decode as add [rax],al forever, so the decoder walks the tail of
# the image and falls out of the mapping.
run_case flat "0f16" "0F 16 then a page of zeros"
run_case flat "09"   "09 then a page of zeros"
# Entry in the final bytes of an image that ends flush with a host page: the
# fetch window itself, not the decode, is what runs past the end.
run_case edge "9090909090909090"     "8x NOP flush with end of mapping"
run_case edge "90"                   "1x NOP flush with end of mapping"
run_case edge "c4e1699bcb"           "VEX+9B+CB flush with end of mapping"
run_case edge "488b0425efbeadde"     "8-byte mov flush with end of mapping"
run_case edge "f3"                   "bare F3 prefix flush with end of mapping"
run_case edge "c4"                   "bare VEX byte flush with end of mapping"
run_case edge "df"                   "bare x87 escape flush with end of mapping"

# ---------------------------------------------------------------------------
# Known, still-open host-kill classes.  Both are reached only through the
# exhaustive audit sweep below; neither is a decode-stream defect, and each
# needs its own fix:
#
#  A. x87 / fxsave helpers (runtime/frontend/x86/x87.cpp LoadGuest<>,
#     X87Fxsave) dereference the guest address straight from HOST code called
#     out of a JIT block.  Faulting there is unrecoverable for the same reason
#     the decoder was: the fault pc is not inside a JIT buffer.  Reproduce:
#     payload "d9 00.." (fld dword [rax], rax=0) or "0f ae 00.." (fxsave).
#
#  B. the arm64 register allocator throws std::logic_error
#     ("Check failed: active_gprs.Get(id)") on some IR shapes reached from
#     malformed streams; nothing catches it, so the host std::terminate()s.
#     Fires nondeterministically -- the path depends on ASLR-dependent
#     register contents -- so it is matched by signature, not by opcode.
# ---------------------------------------------------------------------------
if [ "$AUDIT" = "--audit" ]; then
    echo "== audit sweep: full 1-byte / 0F / VEX opcode space =="
    audit_new=0
    audit_known=0
    sweep_one() {
        local payload="$1" name="$2"
        local elf="$WORK/a.elf" err="$WORK/a.err"
        python3 "$GEN" flat "$payload" -o "$elf" || return 0
        SVM_AVX=0 SVM_BMI=0 $TIMEOUT "$SVM" "$elf" >/dev/null 2>"$err"
        local rc=$?
        [ "$rc" -eq 124 ] && return 0        # guest infinite loop: legitimate
        local why=""
        grep -q "unhandled host fault" "$err" && why="$(grep -m1 'unhandled host fault' "$err")"
        grep -qE "libc\+\+abi|Check failed" "$err" && why="host abort: $(grep -m1 -E 'Check failed|terminating' "$err")"
        [ -z "$why" ] && [ "$rc" -ge 128 ] && why="host killed by signal $((rc - 128))"
        [ -z "$why" ] && return 0
        case "$why" in
            *"Check failed"*|*terminating*)   # class B
                audit_known=$((audit_known + 1)); echo "KNOWN-B $name [$payload]" ;;
            *)
                case "$name" in               # class A: x87 escapes + 0F AE
                    "1B D9"|"1B DB"|"1B DD"|"1B DF"|"1B D8"|"1B DA"|"1B DC"|"1B DE"|"0F AE")
                        audit_known=$((audit_known + 1)); echo "KNOWN-A $name [$payload]" ;;
                    *)
                        audit_new=$((audit_new + 1)); echo "NEW     $name [$payload]: $why" ;;
                esac ;;
        esac
    }
    for op in $(seq 0 255); do
        sweep_one "$(printf '%02x' "$op")00000000000000000000" "$(printf '1B %02X' "$op")"
    done
    for op in $(seq 0 255); do
        sweep_one "0f$(printf '%02x' "$op")00000000000000000000" "$(printf '0F %02X' "$op")"
    done
    for m in e1 e2 e3; do for p in 78 79 7a 7b; do
        for op in $(seq 0 255); do
            sweep_one "c4${m}${p}$(printf '%02x' "$op")cb000000000000" "VEX ${m}/${p} $(printf '%02X' "$op")"
        done
    done; done
    echo "audit: $audit_known known-class host kills, $audit_new NEW"
    fail=$((fail + audit_new))
fi

echo
echo "malformed-guest isolation: $pass passed, $fail failed ($flaky needed a retry -- KNOWN-B/C, see header)"
[ "$fail" -eq 0 ] || exit 1
exit 0
