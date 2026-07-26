#!/usr/bin/env bash
# build_avx_real_tests.sh -- rebuild the real-AVX-program guest binaries and
# regenerate their golden files.
#
# This is the AVX sibling of build_real_tests.sh.  It differs in two ways that
# matter:
#
#  1. The guests are freestanding (-nostdlib, own _start).  Not minimalism: a
#     static glibc reaches its AVX2 memcpy/strlen through an ifunc that also
#     requires BMI2, which SwiftVM does not implement, so a libc-based guest
#     would never execute the AVX2 code paths under test.  Here every AVX2
#     instruction is unconditional.
#
#  2. The golden values come from RUNNING an x86-64 build of the very same
#     kernels, never from hand computation.  On Apple Silicon that run happens
#     under Rosetta (arch -x86_64); on an x86-64 Linux box it is just native.
#
# ---------------------------------------------------------------------------
# HOW TO REGENERATE THE GOLDEN FILES
# ---------------------------------------------------------------------------
#   bash source/translator/linux/tests/build_avx_real_tests.sh
#
# That rebuilds avx_real_x86_64 / avx_crosspage_x86_64 and overwrites
# avx_real_x86_64.native.txt / avx_crosspage_x86_64.native.txt with the actual
# stdout+exit status of the native oracle.  Review the diff before keeping it:
# a changed golden means either the kernels changed or the ORACLE changed, and
# the oracle is not trustworthy by default (see the Rosetta notes below).
#
# ---------------------------------------------------------------------------
# ORACLE CAVEATS -- read before trusting a green run
# ---------------------------------------------------------------------------
#  * Rosetta's CPUID hides AVX unless ROSETTA_ADVERTISE_AVX=1.  These binaries
#    never ask: they are compiled with unconditional -mavx2, so "can the oracle
#    do AVX2" is answered by the program running at all, not by a feature bit.
#    ROSETTA_ADVERTISE_AVX=1 is still exported below, for any libc that peeks.
#  * Rosetta is a KNOWN-DEFECTIVE oracle.  It cannot deliver a synchronous
#    page fault taken inside a 256-bit access that straddles a page boundary:
#    it aborts the process with
#      "rosetta error: unexpectedly need to EmulateForward on a synchronous
#       exception ... 0xc5fc1107" (that is vmovups %ymm0,(%rdi))
#    A scalar store, a 16-byte SSE store, and a 32-byte store landing wholly
#    inside the unmapped page are all handled correctly -- only the straddling
#    32-byte case aborts.  Cross-page stages 1/2/4 therefore have NO oracle
#    here and are recorded as such rather than being given a made-up value.
#  * Where an oracle and the Intel SDM disagree, the SDM wins and the
#    disagreement gets written down.  A single green oracle is not evidence.
#
set -euo pipefail

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$TESTS_DIR"

HOST_ARCH="$(uname -m)"
KERNEL="$(uname -s)"

# --------------------------------------------------------------------------
# 1. guest binaries: static Linux x86-64 ELF, freestanding
# --------------------------------------------------------------------------
# -fno-jump-tables is a WORKAROUND, not a preference.  With jump tables on,
# clang lowers avx_crosspage's `switch (stage)` to
#     jmpq *disp32(,%rax,8)
# i.e. an indirect jump whose memory operand is a SIB with an index but NO
# base register, and that form takes SwiftVM's x86 frontend down: the whole
# translator process dies with an unhandled host SIGSEGV/SIGBUS (or trips the
# internal `Check failed: "active_gprs.Get(id)"`), never reaching the guest.
# It is not an AVX problem -- a pure-scalar switch reproduces it, and so does
# a three-instruction hand-built ELF:
#     jmp *0x400800(,%rax,8)   with rax=0   -> translator SIGSEGVs
#     jmp *(%rbx,%rax,8)                    -> fine
#     jmp *0x400800                         -> fine
# Every real compiler emits the broken form for any switch of a handful of
# dense cases, so this deserves a fix in the frontend; until then the AVX
# tests cannot use jump tables and still measure AVX.
GUEST_CFLAGS="-fno-pic -fno-stack-protector -fno-jump-tables -mavx2 -O2"

build_guest() {
    local src="$1" out="$2"
    if command -v x86_64-linux-gnu-gcc >/dev/null; then
        x86_64-linux-gnu-gcc -static -nostdlib -Wl,--build-id=none \
            $GUEST_CFLAGS -o "$out" "$src"
    elif [ "$KERNEL" = Linux ] && [ "$HOST_ARCH" = x86_64 ]; then
        gcc -static -nostdlib -Wl,--build-id=none $GUEST_CFLAGS -o "$out" "$src"
    else
        # No ELF linker on this host (typical for a macOS workstation): clang
        # still cross-COMPILES to ELF, and mklinuxelf.py does the linking.
        local res
        res="$(clang -print-resource-dir)"
        clang -target x86_64-unknown-linux-gnu -ffreestanding -nostdinc \
            -isystem "$res/include" -D__MM_MALLOC_H \
            $GUEST_CFLAGS -c "$src" -o "$out.o"
        python3 "$TESTS_DIR/mklinuxelf.py" -o "$out" "$out.o"
        rm -f "$out.o"
    fi
    chmod +x "$out"
}

echo "== building guests =="
build_guest avx_real_x86_64.c      avx_real_x86_64
build_guest avx_crosspage_x86_64.c avx_crosspage_x86_64

# --------------------------------------------------------------------------
# 2. native oracle: same kernels, x86-64, run for real
# --------------------------------------------------------------------------
echo "== building oracle =="
ORACLE_DIR="${TMPDIR:-/tmp}/svm_avx_oracle.$$"
mkdir -p "$ORACLE_DIR"
trap 'rm -rf "$ORACLE_DIR"' EXIT

if [ "$KERNEL" = Darwin ]; then
    clang -arch x86_64 -mavx2 -O2 -o "$ORACLE_DIR/avx_real_host"      avx_real_host.c
    clang -arch x86_64 -mavx2 -O2 -o "$ORACLE_DIR/avx_crosspage_host" avx_crosspage_host.c
    RUN_X86=(env ROSETTA_ADVERTISE_AVX=1 arch -x86_64)
elif [ "$HOST_ARCH" = x86_64 ]; then
    gcc -mavx2 -O2 -o "$ORACLE_DIR/avx_real_host"      avx_real_host.c
    gcc -mavx2 -O2 -o "$ORACLE_DIR/avx_crosspage_host" avx_crosspage_host.c
    RUN_X86=(env)
else
    echo "no way to run x86-64 code on this host; cannot regenerate goldens" >&2
    exit 1
fi

record() {
    local binary="$1" output="$2"; shift 2
    : >"$output"
    for arg in "$@"; do
        set +e
        local text status
        text="$("${RUN_X86[@]}" "$binary" "$arg" 2>/dev/null)"
        status=$?
        set -e
        printf '[%s]\n' "$arg" >>"$output"
        [ -n "$text" ] && printf '%s\n' "$text" >>"$output"
        printf 'exit=%d\n' "$status" >>"$output"
    done
    echo "oracle $(basename "$binary"): $output"
}

# avx_real: every kernel on its own (so one fatal decode gap does not hide the
# other ten) plus the whole-program run.
record "$ORACLE_DIR/avx_real_host" avx_real_x86_64.native.txt \
    0 1 2 3 4 5 6 7 8 9 10 all

# avx_crosspage: stage 0 is the comparable control; 1/2/4 abort Rosetta and are
# recorded exactly as observed, aborts and all.
record "$ORACLE_DIR/avx_crosspage_host" avx_crosspage_x86_64.native.txt \
    0 1 2 3 4 5

echo "== done =="
ls -l avx_real_x86_64 avx_crosspage_x86_64 avx_*.native.txt
