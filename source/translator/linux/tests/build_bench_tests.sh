#!/usr/bin/env bash
# build_bench_tests.sh -- rebuild the performance-baseline guest binary and its
# golden checksum file.
#
# Sibling of build_avx_real_tests.sh; same freestanding cross recipe, same rule
# about goldens: the values in bench_suite_x86_64.native.txt come from RUNNING
# an x86-64 build of the very same kernels (under Rosetta on Apple Silicon),
# never from hand computation.  They are a correctness anchor for the benchmark
# --- a performance change that alters a checksum is a bug, not a speedup.
#
#   bash source/translator/linux/tests/build_bench_tests.sh
#
# The guest is freestanding (-nostdlib, own _start) on purpose: a static-glibc
# benchmark would spend most of a short run inside libc startup, and would
# therefore measure the cost of compiling glibc rather than the cost of the
# kernel under test.
#
# -fno-jump-tables: inherited from build_avx_real_tests.sh, which documents a
# frontend defect on `jmp *disp32(,%rax,8)` (SIB with index and no base).  The
# benchmark has no switch statements, so this only guards against the compiler
# inventing one.
set -euo pipefail

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$TESTS_DIR"

HOST_ARCH="$(uname -m)"
KERNEL="$(uname -s)"

GUEST_CFLAGS="-fno-pic -fno-stack-protector -fno-jump-tables -O2"

build_guest() {
    local src="$1" out="$2"
    if command -v x86_64-linux-gnu-gcc >/dev/null; then
        x86_64-linux-gnu-gcc -static -nostdlib -Wl,--build-id=none \
            $GUEST_CFLAGS -o "$out" "$src"
    elif [ "$KERNEL" = Linux ] && [ "$HOST_ARCH" = x86_64 ]; then
        gcc -static -nostdlib -Wl,--build-id=none $GUEST_CFLAGS -o "$out" "$src"
    else
        # No ELF linker on this host (typical macOS workstation): clang still
        # cross-COMPILES to ELF and mklinuxelf.py does the linking.
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

echo "== building guest =="
build_guest bench_suite_x86_64.c bench_suite_x86_64

# --------------------------------------------------------------------------
# golden checksums: the same kernels, executed as real x86-64 code
# --------------------------------------------------------------------------
echo "== building native oracle =="
ORACLE_DIR="${TMPDIR:-/tmp}/svm_bench_oracle.$$"
mkdir -p "$ORACLE_DIR"
trap 'rm -rf "$ORACLE_DIR"' EXIT

if [ "$KERNEL" = Darwin ]; then
    # Compile the guest source for macOS x86-64 and run it under Rosetta.  The
    # guest's own _start/syscall stubs are compiled out by SVM_BENCH_NATIVE.
    clang -arch x86_64 -O2 -DSVM_BENCH_NATIVE=1 \
        -o "$ORACLE_DIR/bench_native" bench_suite_native.c
    RUN=(arch -x86_64 "$ORACLE_DIR/bench_native")
elif [ "$HOST_ARCH" = x86_64 ]; then
    clang -O2 -DSVM_BENCH_NATIVE=1 -o "$ORACLE_DIR/bench_native" bench_suite_native.c
    RUN=("$ORACLE_DIR/bench_native")
else
    echo "no x86-64 oracle available on $KERNEL/$HOST_ARCH -- keeping existing golden" >&2
    exit 0
fi

echo "== recording golden checksums =="
{
    echo "# bench_suite_x86_64 golden checksums"
    echo "# produced by build_bench_tests.sh from a real x86-64 execution"
    echo "# (arch -x86_64 on Apple Silicon); scale=1 for every kernel."
    "${RUN[@]}" all 1
    echo "exit=$?"
} > bench_suite_x86_64.native.txt

cat bench_suite_x86_64.native.txt
echo "== done =="
