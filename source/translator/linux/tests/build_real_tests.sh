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

# --- musl static (x86_64: apt-get install musl-tools) ---
if [ "$SUFFIX" = x86_64 ] && command -v musl-gcc >/dev/null; then
    musl-gcc -static -O2 -o real_hello_musl_x86_64 real_hello.c
    musl-gcc -static -O2 -o real_busy_musl_x86_64  real_busy.c
fi

# --- verify ---
file "real_hello_${SUFFIX}" "real_busy_${SUFFIX}"
readelf -h "real_hello_${SUFFIX}" | grep -E 'Type|Machine|Entry'

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
ls -l real_hello_* real_busy_* 2>/dev/null || true
