#!/usr/bin/env bash
# Builds the static guest programs consumed by run_programs.py.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here/bin}"
cc="${CC:-cc}"
arch="$(uname -m)"
mkdir -p "$out"

flags=(-static -no-pie -O2)
for program in hello syscalls compute; do
    "$cc" "${flags[@]}" "$here/src/$program.c" -o "$out/$program"
done
"$cc" "${flags[@]}" "$here/src/fp.c" -o "$out/fp" -lm
"$cc" "${flags[@]}" "$here/src/bench_int.c" -o "$out/bench_int"
"$cc" "${flags[@]}" "$here/src/bench_fp.c" -o "$out/bench_fp"

case "$arch" in
    aarch64 | arm64) tiny=tiny_arm64.S ;;
    x86_64 | amd64) tiny=tiny_x64.S ;;
    *) tiny= ;;
esac
if [ -n "$tiny" ]; then
    "$cc" -nostdlib -static -no-pie "$here/src/$tiny" -o "$out/tiny"
fi
