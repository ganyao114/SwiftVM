#!/usr/bin/env bash
# Rebuilds the guest ABI stub ELF (abi_stubs_x86_64) from abi_stubs.c.
#
# clang on an Apple Silicon box cross-COMPILES to x86-64 ELF but there is no
# ELF linker, so mklinuxelf.py (source/translator/linux/tests) does the link --
# the same arrangement build_bench_tests.sh uses.
#
# The result is checked in, so this only needs running when abi_stubs.c changes.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MKELF="$HERE/../../linux/tests/mklinuxelf.py"
OUT="$HERE/abi_stubs_x86_64"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

clang --target=x86_64-unknown-linux-gnu \
      -ffreestanding -nostdlib -fno-pic -fno-pie -mcmodel=small \
      -fno-stack-protector -fno-builtin -O2 \
      -I"$HERE" -c "$HERE/abi_stubs.c" -o "$TMP/abi_stubs.o"

python3 "$MKELF" -o "$OUT" "$TMP/abi_stubs.o" --base 0x10000000 --entry _start
echo "wrote $OUT"
