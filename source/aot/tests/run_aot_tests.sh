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

BUILD=${1:?usage: run_aot_tests.sh <build-dir>}
AOT="$BUILD/source/aot/svm_aot"
REF="$BUILD/source/translator/linux/svm_translator_linux"
HERE="$(cd "$(dirname "$0")" && pwd)"
CORPUS="$(cd "$HERE/../../translator/linux/tests" && pwd)"
WORK="${AOT_TEST_WORK:-$(mktemp -d)}"
mkdir -p "$WORK"

pass=0; fail=0
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
head_() { printf '\n== %s ==\n' "$1"; }

[ -x "$AOT" ] || { echo "missing $AOT"; exit 2; }

# --------------------------------------------------------------------------
head_ "0. build the guest-globals case"
# --------------------------------------------------------------------------
if clang --target=x86_64-unknown-linux-gnu -ffreestanding -nostdlib -fno-pic \
        -fno-pie -mcmodel=small -O1 -fno-stack-protector \
        -c "$HERE/globals_guest.c" -o "$WORK/globals_guest.o" 2>/dev/null &&
   python3 "$HERE/mkaotguest.py" -o "$HERE/globals_guest_x86_64" \
        "$WORK/globals_guest.o" --entry _start >/dev/null; then
    ok "globals_guest_x86_64 rebuilt"
else
    if [ -f "$HERE/globals_guest_x86_64" ]; then
        ok "globals_guest_x86_64 (using the checked-in copy; clang cross unavailable)"
    else
        bad "cannot build globals_guest_x86_64"
    fi
fi

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

run_equivalence globals "$HERE/globals_guest_x86_64"
run_equivalence func_tests "$CORPUS/func_tests_x86_64"
run_equivalence real_busy "$CORPUS/real_busy_x86_64"

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
# hash (which is checked first). SVM_X87_TOPVIRT changes register reservation
# inside TranslateIR without touching Config, so it can only be caught by the
# environment hash -- which is exactly why code_serial hashes raw SVM_*/SWIFT_*
# strings instead of interpreted semantics.
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
printf '\n== summary ==\n  %d passed, %d failed  (work dir: %s)\n' "$pass" "$fail" "$WORK"
[ "$fail" = 0 ]
