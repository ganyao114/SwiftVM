#!/usr/bin/env bash
# build_avx_hdbg.sh -- build the K0 fdot bisect harness (guest + oracle) and
# diff SwiftVM against the x86-64 oracle line by line.
#
#   bash build_avx_hdbg.sh <path-to-svm_translator_linux>
#
# Same recipe as build_avx_real_tests.sh; kept separate because this harness is
# a debugging instrument, not a qualification suite.
set -euo pipefail
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$TESTS_DIR"
SVM="${1:-}"
OUT="${TMPDIR:-/tmp}/svm_avx_hdbg"
mkdir -p "$OUT"

res="$(clang -print-resource-dir)"
clang -target x86_64-unknown-linux-gnu -ffreestanding -nostdinc \
    -isystem "$res/include" -D__MM_MALLOC_H \
    -fno-pic -fno-stack-protector -fno-jump-tables -mavx2 -O2 \
    -c avx_hdbg_x86_64.c -o "$OUT/g.o"
python3 "$TESTS_DIR/mklinuxelf.py" -o avx_hdbg_x86_64 "$OUT/g.o"
chmod +x avx_hdbg_x86_64

clang -arch x86_64 -mavx2 -O2 -o "$OUT/avx_hdbg_host" avx_hdbg_host.c

echo "== oracle (x86-64) =="
ROSETTA_ADVERTISE_AVX=1 arch -x86_64 "$OUT/avx_hdbg_host" >"$OUT/oracle.txt" 2>&1 || true
cat "$OUT/oracle.txt"

if [ -n "$SVM" ]; then
    echo "== SwiftVM =="
    SVM_AVX=1 "$SVM" avx_hdbg_x86_64 >"$OUT/svm.txt" 2>&1 || true
    cat "$OUT/svm.txt"
    echo "== diff (< oracle, > svm) =="
    # SwiftVM prints two host-side mapping notes on stderr; they are not results.
    grep -v 'fixed map failed\|stack placed' "$OUT/svm.txt" >"$OUT/svm.clean"
    if diff "$OUT/oracle.txt" "$OUT/svm.clean"; then
        echo "VERDICT: identical to x86-64 hardware"
    else
        echo "VERDICT: MISMATCH (lines above; '<' is hardware, '>' is SwiftVM)"
        exit 1
    fi
fi
