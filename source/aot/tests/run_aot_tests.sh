#!/usr/bin/env bash
#
# AOT end-to-end suite (docs/aot-design.md §7).
#
#   run_aot_tests.sh <build-dir>
#
# Corpus is func_tests_x86_64 / real_busy_x86_64 on purpose: they are the only
# guests with a .symtab, so they are the only ones that exercise "preserve the
# symbols" and "fix up the symbol table". globals_guest_x86_64 is built here
# and is the dedicated guest-global-data case.
#
# Every "must be rejected" case is checked for a *specific* rejection, not just
# a non-zero exit: a test that passes because the binary crashed for an
# unrelated reason is worse than no test.
#
set -u
export PYTHONDONTWRITEBYTECODE=1

BUILD_ARG=${1:?usage: run_aot_tests.sh <build-dir>}
BUILD="$(cd "$BUILD_ARG" && pwd)" || {
    echo "missing build directory: $BUILD_ARG"
    exit 2
}
AOT="$BUILD/source/aot/svm_aot"
REF="$BUILD/source/translator/linux/svm_translator_linux"
HERE="$(cd "$(dirname "$0")" && pwd)"
CORPUS="$(cd "$HERE/../../translator/linux/tests" && pwd)"
WORK_ARG="${AOT_TEST_WORK:-$(mktemp -d)}"
mkdir -p "$WORK_ARG"
WORK="$(cd "$WORK_ARG" && pwd)"

pass=0; fail=0
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
head_() { printf '\n== %s ==\n' "$1"; }

[ -x "$AOT" ] || { echo "missing $AOT"; exit 2; }

# --------------------------------------------------------------------------
head_ "0. build the guest-globals case"
# --------------------------------------------------------------------------
build_guest() {
    local name=$1
    local output="$WORK/${name}_x86_64"
    if clang --target=x86_64-unknown-linux-gnu -ffreestanding -nostdlib -fno-pic \
            -fno-pie -mcmodel=small -O1 -fno-stack-protector \
            -c "$HERE/$name.c" -o "$WORK/$name.o" 2>/dev/null &&
       python3 "$HERE/mkaotguest.py" -o "$output" \
            "$WORK/$name.o" --entry _start >/dev/null; then
        ok "${name}_x86_64 rebuilt"
    elif [ -f "$HERE/${name}_x86_64" ]; then
        cp "$HERE/${name}_x86_64" "$output"
        ok "${name}_x86_64 (copied checked-in fixture; clang cross unavailable)"
    else
        bad "cannot build ${name}_x86_64"
    fi
}
build_guest globals_guest
build_guest smc_guest

# --------------------------------------------------------------------------
# equivalence: JIT vs AOT, byte for byte, same runner
# --------------------------------------------------------------------------
run_equivalence() {
    local name=$1 guest=$2
    head_ "equivalence: $name"
    if ! "$AOT" compile "$guest" -o "$WORK/$name.aot" > "$WORK/$name.compile" 2>&1; then
        bad "$name: compile"; cat "$WORK/$name.compile"; return
    fi
    sed -n 's/^/    /p' "$WORK/$name.compile" | tail -5

    "$AOT" run --guest "$guest" > "$WORK/$name.jit.out" 2> "$WORK/$name.jit.err"
    local jit_rc=$?
    "$AOT" run --stats --aot "$WORK/$name.aot" > "$WORK/$name.aot.out" 2> "$WORK/$name.aot.err"
    local aot_rc=$?
    local units
    units=$(sed -n 's/.*installed \([0-9]*\) units.*/\1/p' "$WORK/$name.aot.err")

    if [ "$jit_rc" = "$aot_rc" ]; then ok "$name: exit code $aot_rc"
    else bad "$name: exit code jit=$jit_rc aot=$aot_rc"; fi
    if cmp -s "$WORK/$name.jit.out" "$WORK/$name.aot.out"; then
        ok "$name: stdout byte-for-byte identical ($(wc -c < "$WORK/$name.jit.out" | tr -d ' ') bytes, ${units:-0} AOT units)"
    else
        bad "$name: stdout differs"; diff "$WORK/$name.jit.out" "$WORK/$name.aot.out" | head -10
    fi

    # The artifact is self-contained: guest memory came from its own PT_LOAD
    # copies. Cross-check the shared runner against the real launcher too, so
    # "identical" cannot mean "both equally wrong".
    if [ -x "$REF" ]; then
        ( cd "$(dirname "$guest")" && "$REF" "$guest" > "$WORK/$name.ref.out" 2>/dev/null )
        if cmp -s "$WORK/$name.ref.out" "$WORK/$name.jit.out"; then
            ok "$name: runner matches svm_translator_linux"
        else
            bad "$name: runner differs from svm_translator_linux"
        fi
    fi

    head_ "artifact structure: $name"
    if python3 "$HERE/aot_elf_check.py" "$WORK/$name.aot" --guest "$guest" \
            > "$WORK/$name.elfcheck" 2>&1; then
        sed -n 's/^/    /p' "$WORK/$name.elfcheck" | grep -v '^    $'
        ok "$name: artifact passes the independent ELF parser"
    else
        bad "$name: artifact fails the independent ELF parser"
        cat "$WORK/$name.elfcheck"
    fi
}

run_equivalence globals "$WORK/globals_guest_x86_64"
run_equivalence func_tests "$CORPUS/func_tests_x86_64"
run_equivalence real_busy "$CORPUS/real_busy_x86_64"

# --------------------------------------------------------------------------
head_ "coverage: how much run-time JIT the artifact actually removes"
# --------------------------------------------------------------------------
# An artifact that loads and produces the right output can still leave almost
# all of the translation work to run time, which is what the default (lazy,
# one block per unit) does. --dump-compiles counts what the JIT still had to
# translate WITH the artifact installed, so the claim is measured rather than
# assumed. Both wider settings must stay byte-identical to the JIT baseline:
# more coverage that changes the output is not coverage, it is a bug.
coverage() {
    local name=$1 guest=$2 want_max=$3; shift 3
    "$AOT" run --guest "$guest" --dump-compiles "$WORK/$name.jitonly.log" \
        > "$WORK/$name.base.out" 2> "$WORK/$name.base.err"
    local base_rc=$?
    local base; base=$(wc -l < "$WORK/$name.jitonly.log" | tr -d ' ')

    if ! "$AOT" compile "$guest" -o "$WORK/$name.cov.aot" "$@" \
            > "$WORK/$name.cov.compile" 2>&1; then
        bad "$name: compile $*"; cat "$WORK/$name.cov.compile"; return
    fi
    "$AOT" run --aot "$WORK/$name.cov.aot" --dump-compiles "$WORK/$name.cov.log" \
        > "$WORK/$name.cov.out" 2> "$WORK/$name.cov.err"
    local cov_rc=$?
    local left; left=$(wc -l < "$WORK/$name.cov.log" | tr -d ' ')
    local units; units=$(sed -n 's/.*units emitted *: *//p' "$WORK/$name.cov.compile")

    if [ "$base_rc" = "$cov_rc" ] && cmp -s "$WORK/$name.base.out" "$WORK/$name.cov.out"; then
        ok "$name [$*]: still byte-identical to the JIT baseline"
    else
        bad "$name [$*]: output or exit code changed (jit=$base_rc aot=$cov_rc)"
        diff "$WORK/$name.base.out" "$WORK/$name.cov.out" | head -5
    fi
    if [ "${left:-999999}" -le "$want_max" ]; then
        ok "$name [$*]: $base run-time translations -> $left (<= $want_max), $units units"
    else
        bad "$name [$*]: $base -> $left run-time translations, expected <= $want_max"
    fi
}

# Budgets are the measured numbers with slack. The residual is dominated by
# indirect jump-table targets (switch_worker, __printf_buffer,
# read_encoded_value_with_base) plus the block after a `syscall`, which the
# design deliberately leaves to the dispatcher.
coverage func_tests "$CORPUS/func_tests_x86_64" 800 --eager
coverage func_tests_sweep "$CORPUS/func_tests_x86_64" 200 --eager --sweep
coverage real_busy_sweep "$CORPUS/real_busy_x86_64" 250 --eager --sweep

head_ "artifact structure: eager+sweep"
if python3 "$HERE/aot_elf_check.py" "$WORK/func_tests_sweep.cov.aot" \
        --guest "$CORPUS/func_tests_x86_64" > "$WORK/sweep.elfcheck" 2>&1; then
    sed -n 's/^/    /p' "$WORK/sweep.elfcheck" | grep -v '^    $'
    ok "eager+sweep artifact passes the independent ELF parser"
else
    bad "eager+sweep artifact fails the independent ELF parser"
    cat "$WORK/sweep.elfcheck"
fi

BASE="$WORK/func_tests.aot"
GUEST="$CORPUS/func_tests_x86_64"
GOOD_OUT="$WORK/func_tests.jit.out"

# --------------------------------------------------------------------------
head_ "rejection paths (docs/aot-design.md §7.5)"
# --------------------------------------------------------------------------
mutate() {
    # A mutation that fails to apply would leave the previous (or no) file
    # behind and every downstream check would "pass" for the wrong reason.
    if ! python3 "$HERE/aot_mutate.py" "$@" > "$WORK/mutate.log" 2>&1; then
        bad "mutation $3 could not be applied"; sed -n 's/^/      /p' "$WORK/mutate.log"
        return 1
    fi
    return 0
}

# Each expects a *named* rejection reason, so a crash cannot pass as a reject.
expect_reject() {
    local label=$1 needle=$2; shift 2
    local out rc
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" != 0 ] && printf '%s' "$out" | grep -q "$needle"; then
        ok "$label -> rejected ($(printf '%s' "$out" | grep -o "$needle" | head -1))"
    else
        bad "$label -> NOT rejected (rc=$rc): $(printf '%s' "$out" | head -2)"
    fi
}

# (a) one byte of the guest image changed
mutate "$BASE" "$WORK/m_guest.aot" guest-byte
expect_reject "guest image changed (1 byte)" "not what unit" \
    "$AOT" run --aot "$WORK/m_guest.aot"
# the same, against an on-disk guest that no longer matches
cp "$GUEST" "$WORK/tampered_guest"
python3 - "$WORK/tampered_guest" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
d[0x2000] ^= 0x01
open(p, 'wb').write(bytes(d))
PY
expect_reject "guest ELF on disk changed" "does not match" \
    "$AOT" run --aot "$BASE" --guest "$WORK/tampered_guest"

# (b) a switch that participates in the hash
# SVM_STATIC_REGS / SVM_TSO_MODE reach Config, so they trip the *config*
# hash (which is checked first). SVM_X87_TOPVIRT remains in the environment
# identity for artifact compatibility after its dedicated-register path was
# retired, so this case directly exercises the raw SVM_*/SWIFT_* hash.
expect_reject "SVM_STATIC_REGS=0 at run time" "Config differs" \
    env SVM_STATIC_REGS=0 "$AOT" run --aot "$BASE"
expect_reject "SVM_TSO_MODE=acqrel at run time" "Config differs" \
    env SVM_TSO_MODE=acqrel "$AOT" run --aot "$BASE"
expect_reject "SVM_X87_TOPVIRT=1 at run time (env hash only)" "environment differs" \
    env SVM_X87_TOPVIRT=1 "$AOT" run --aot "$BASE"

# (c) corrupted artifact
mutate "$BASE" "$WORK/m_info.aot" info-byte
expect_reject "corrupt .svmaot.info" "checksum" "$AOT" run --aot "$WORK/m_info.aot"
mutate "$BASE" "$WORK/m_codeb.aot" code-byte-nostamp
expect_reject "corrupt .svmaot.text" "recorded hash" "$AOT" run --aot "$WORK/m_codeb.aot"

# (d) a different SwiftVM build
python3 - "$BASE" "$WORK/m_build.aot" <<'PY'
import struct, sys
sys.path.insert(0, __file__ and "")
d = bytearray(open(sys.argv[1], 'rb').read())
# .svmaot.info magic is unique in the file; the build id is the 2nd u64 after it
i = d.find(b"SVMAOT\x00\x01")
off = i + 8 + 8 + 8            # magic + format + key.format_version
struct.pack_into("<Q", d, off, struct.unpack_from("<Q", d, off)[0] ^ 1)
open(sys.argv[2], 'wb').write(bytes(d))
PY
expect_reject "different SwiftVM build id" "different SwiftVM build" \
    "$AOT" run --aot "$WORK/m_build.aot"

# --------------------------------------------------------------------------
head_ "mutation testing (docs/aot-design.md §7.6)"
# --------------------------------------------------------------------------
# A mutation "is caught" when the run's observable behaviour changes. Because
# every rejection above is a hard failure (exit 70) rather than a silent
# fallback to JIT, these cannot pass for the wrong reason.
expect_behaviour_change() {
    local label=$1 art=$2
    local rc
    "$AOT" run --aot "$art" > "$WORK/mut.out" 2> "$WORK/mut.err"; rc=$?
    if [ "$rc" = 101 ] && cmp -s "$WORK/mut.out" "$GOOD_OUT"; then
        bad "$label -> NOT caught (identical output and exit code)"
    else
        ok "$label -> caught (rc=$rc, $(head -c 60 "$WORK/mut.err" | tr -d '\n')$(cmp -s "$WORK/mut.out" "$GOOD_OUT" || echo 'output differs'))"
    fi
}

# M1: relocations written back to the wrong host address.
mutate "$BASE" "$WORK/m_reloc.aot" reloc-addend
expect_behaviour_change "relocation addends perturbed" "$WORK/m_reloc.aot"

# M2: the compiled code itself is what runs. Flip one bit of .svmaot.text and
# re-stamp the code hash so the artifact still loads.
mutate "$BASE" "$WORK/m_code.aot" code-byte
expect_behaviour_change "one bit of the AOT code (hash re-stamped)" "$WORK/m_code.aot"

# M3: guest data moved off its linked guest address. Segment 2 is .rodata:
# moving it leaves the image span (and therefore every structural check)
# intact, so the only thing that can notice is the guest reading its own
# constants from the wrong address.
mutate "$BASE" "$WORK/m_move.aot" move-data 2
expect_behaviour_change "guest data segment shifted by one page" "$WORK/m_move.aot"

# M4: symbol sizes wrong -- a static defect, caught by the structural check.
mutate "$BASE" "$WORK/m_size.aot" sym-size
if python3 "$HERE/aot_elf_check.py" "$WORK/m_size.aot" --guest "$GUEST" >/dev/null 2>&1; then
    bad "STT_FUNC st_size grown by 8 -> NOT caught"
else
    ok "STT_FUNC st_size grown by 8 -> caught by aot_elf_check.py"
fi

# --------------------------------------------------------------------------
head_ "self-modifying guest (docs/aot-design.md §8)"
# --------------------------------------------------------------------------
# smc_guest overwrites the immediate inside one of its own functions and calls
# it again. The value it prints is the decision procedure: the OLD one means
# stale artifact code ran (exit 72), the new one means the write was noticed
# and the unit was retranslated (exit 88). §8 asked for "refuse to continue";
# what is implemented is retranslation, which is the same safety property and
# the only one compatible with §7.1's "byte-identical to JIT on all 25 e2e
# guests" -- the doc has been corrected to say so.
SMC_GUEST="$WORK/smc_guest_x86_64"
if [ -f "$SMC_GUEST" ]; then
    "$AOT" run --guest "$SMC_GUEST" > "$WORK/smc.jit.out" 2>&1; smc_jit_rc=$?
    if "$AOT" compile "$SMC_GUEST" -o "$WORK/smc.aot" --eager --sweep \
            > "$WORK/smc.compile" 2>&1; then
        "$AOT" run --aot "$WORK/smc.aot" > "$WORK/smc.aot.out" 2>&1; smc_aot_rc=$?
        if [ "$smc_aot_rc" = 88 ] && [ "$smc_jit_rc" = 88 ] &&
                cmp -s "$WORK/smc.jit.out" "$WORK/smc.aot.out"; then
            ok "guest overwrote an installed unit -> retranslated, identical to JIT (rc 88)"
        elif [ "$smc_aot_rc" = 72 ]; then
            bad "guest overwrote an installed unit -> STALE AOT CODE RAN (rc 72)"
        else
            bad "smc_guest: jit rc=$smc_jit_rc aot rc=$smc_aot_rc"
            cat "$WORK/smc.aot.out"
        fi
    else
        bad "smc_guest: compile"; cat "$WORK/smc.compile"
    fi
else
    bad "missing $SMC_GUEST"
fi

# --------------------------------------------------------------------------
head_ "AOT <-> call layer, end to end"
# --------------------------------------------------------------------------
# swift_aot_call_test compiles an artifact IN ITS OWN process image (the
# validity key covers the SwiftVM build id, so an artifact from this `svm_aot`
# binary would be refused there -- correctly), installs it, resolves libc
# symbols through the artifact's rewritten .symtab and calls them with GuestFn.
CALLTEST="$BUILD/source/aot/swift_aot_call_test"
if [ -x "$CALLTEST" ]; then
    if "$CALLTEST" > "$WORK/call_test.log" 2>&1; then
        ok "swift_aot_call_test: $(tail -2 "$WORK/call_test.log" | head -1)"
    else
        bad "swift_aot_call_test failed"
        tail -25 "$WORK/call_test.log"
    fi
else
    bad "missing $CALLTEST"
fi

# --------------------------------------------------------------------------
printf '\n== summary ==\n  %d passed, %d failed  (work dir: %s)\n' "$pass" "$fail" "$WORK"
[ "$fail" = 0 ]
