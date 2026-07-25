#!/usr/bin/env bash
# build_real_tests.sh — rebuild the "real" static test binaries for SwiftVM.
#
# Run this script INSIDE an OrbStack Ubuntu machine whose CPU matches the
# target ISA (home dir is auto-mounted at the same path):
#
#   orb -m ubuntu-x64 bash source/translator/linux/tests/build_real_tests.sh x86_64
#   orb -m ubuntu     bash source/translator/linux/tests/build_real_tests.sh aarch64
#
# Or run on any Linux machine of the right architecture; the ISA suffix is
# picked automatically from `uname -m` if the argument is omitted.
#
# Requirements: gcc (apt-get install gcc), musl-tools (x86_64 only, for the
# musl variants), strace (for the syscall trace), qemu-user (x86_64 traces on
# an arm64 host, see below).
set -euo pipefail

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$TESTS_DIR"

ARCH="${1:-$(uname -m)}"
case "$ARCH" in
    x86_64|amd64) SUFFIX=x86_64 ;;
    aarch64|arm64) SUFFIX=aarch64 ;;
    *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
esac

echo "== Building for $SUFFIX in $TESTS_DIR =="

# --- glibc static ---
gcc -static -O2 -o "real_hello_${SUFFIX}" real_hello.c
gcc -static -O2 -o "real_busy_${SUFFIX}" real_busy.c
gcc -static -O2 -o "func_tests_${SUFFIX}" func_tests.c
if [ "$SUFFIX" = x86_64 ]; then
    gcc -nostdlib -static -Wl,--build-id=none \
        -o clone_futex_smoke_x86_64 clone_futex_smoke_x86_64.S
    gcc -nostdlib -static -Wl,--build-id=none \
        -o clone_smc_mt_x86_64 clone_smc_mt_x86_64.S
    gcc -nostdlib -static -Wl,--build-id=none \
        -o clone_tso_litmus_x86_64 clone_tso_litmus_x86_64.S
    gcc -nostdlib -static -Wl,--build-id=none \
        -o clone_lock_rmw_x86_64 clone_lock_rmw_x86_64.S
    gcc -nostdlib -static -Wl,--build-id=none \
        -o clone_unaligned_atomic_x86_64 clone_unaligned_atomic_x86_64.S
    gcc -nostdlib -static -Wl,--build-id=none \
        -o tso_unaligned_access_x86_64 tso_unaligned_access_x86_64.S
    gcc -static -O2 -pthread -o pthread_mutex_counter_x86_64 pthread_mutex_counter.c
    gcc -static -O2 -pthread -o tso_spinlock_counter_x86_64 tso_spinlock_counter.c
    gcc -static -O2 -pthread -o tso_litmus_x86_64 tso_litmus.c
    gcc -static -O2 -pthread -o tso_peterson_x86_64 tso_peterson.c
fi

# --- musl static (x86_64: apt-get install musl-tools) ---
if [ "$SUFFIX" = x86_64 ] && command -v musl-gcc >/dev/null; then
    musl-gcc -static -O2 -o real_hello_musl_x86_64 real_hello.c
    musl-gcc -static -O2 -o real_busy_musl_x86_64  real_busy.c
    musl-gcc -static -O2 -o func_tests_musl_x86_64 func_tests.c
    musl-gcc -static -O2 -pthread -o pthread_mutex_counter_musl_x86_64 pthread_mutex_counter.c
    musl-gcc -static -O2 -pthread -o tso_spinlock_counter_musl_x86_64 tso_spinlock_counter.c
    musl-gcc -static -O2 -pthread -o tso_litmus_musl_x86_64 tso_litmus.c
    musl-gcc -static -O2 -pthread -o tso_peterson_musl_x86_64 tso_peterson.c
fi

# --- verify ---
file "real_hello_${SUFFIX}" "real_busy_${SUFFIX}" "func_tests_${SUFFIX}"
readelf -h "real_hello_${SUFFIX}" | grep -E 'Type|Machine|Entry'

# --- native ground truth ---
# Keep stdout and the exact exit status together so qualification never relies
# on a hand-computed checksum. These files are intentionally architecture/libc
# specific even when the numeric result happens to match.
record_native_result() {
    local binary="$1"
    local output="${binary}.native.txt"
    set +e
    "./${binary}" >"${output}"
    local status=$?
    set -e
    printf 'exit=%d\n' "$status" >>"${output}"
    echo "native ${binary}: exit=${status}"
}

record_native_result "func_tests_${SUFFIX}"
if [ "$SUFFIX" = x86_64 ] && [ -x func_tests_musl_x86_64 ]; then
    record_native_result func_tests_musl_x86_64
fi
if [ "$SUFFIX" = x86_64 ]; then
    for binary in \
        clone_futex_smoke_x86_64 \
        clone_smc_mt_x86_64 \
        clone_tso_litmus_x86_64 \
        clone_lock_rmw_x86_64 \
        clone_unaligned_atomic_x86_64 \
        tso_unaligned_access_x86_64 \
        pthread_mutex_counter_x86_64 \
        tso_spinlock_counter_x86_64 \
        tso_litmus_x86_64 \
        tso_peterson_x86_64; do
        record_native_result "$binary"
    done
    for binary in \
        pthread_mutex_counter_musl_x86_64 \
        tso_spinlock_counter_musl_x86_64 \
        tso_litmus_musl_x86_64 \
        tso_peterson_musl_x86_64; do
        if [ -x "$binary" ]; then
            record_native_result "$binary"
        fi
    done
fi

# --- syscall traces ---
# Native strace (works on matching-ISA machines):
if command -v strace >/dev/null; then
    strace -f -o "real_hello_${SUFFIX}.strace.txt" "./real_hello_${SUFFIX}" || true
    strace -f -o "real_busy_${SUFFIX}.strace.txt" "./real_busy_${SUFFIX}" >/dev/null 2>&1 || true
fi

# NOTE for x86_64 traces from an Apple-Silicon host: OrbStack's ubuntu-x64
# machine runs x86 code via Rosetta, and Rosetta does NOT support ptrace, so
# native `strace` only records the exit event. Use qemu-user instead (this is
# how the checked-in *.strace.txt files for x86_64 were produced):
#
#   orb -m ubuntu bash -c 'cd source/translator/linux/tests &&
#     qemu-x86_64 -strace ./real_hello_x86_64 2> real_hello_x86_64.strace.txt'
#
# (qemu-user: apt-get install qemu-user; mantic is EOL, point
# /etc/apt/sources.list at old-releases.ubuntu.com first.)

echo "== Done =="
ls -l real_hello_* real_busy_* func_tests_* 2>/dev/null || true
