#!/bin/bash

# Linux-only launch-policy and identity-collision regression test.

set -u

SVM="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
GUEST="${2:-$HERE/hello_x86_64}"
# hello_x86_64 的既定约定是退出码 42（隔离套件同款）；其它 guest 用第三参传入。
EXPECT_RC="${3:-42}"

if [ -z "$SVM" ] || [ ! -x "$SVM" ] || [ ! -x "$GUEST" ]; then
    echo "usage: $0 <svm_translator_linux> [static-x86_64-guest] [expected-rc]" >&2
    exit 2
fi
if [ "$(uname -s)" != Linux ]; then
    echo "SKIP: Linux identity mapping requires a Linux host"
    exit 0
fi

run_case() {
    name="$1"
    expected="$2"
    shift 2
    out="$(env SVM_MEM_MODE_TRACE=1 "$@" "$SVM" "$GUEST" 2>&1)"
    rc=$?
    if [ "$rc" -ne "$EXPECT_RC" ]; then
        echo "FAIL $name: guest rc=$rc (expected $EXPECT_RC)" >&2
        echo "$out" >&2
        exit 1
    fi
    line="$(printf '%s\n' "$out" | grep '^\[svm-mem-mode\]' | tail -1)"
    if [[ "$line" != *"$expected"* ]]; then
        echo "FAIL $name: expected '$expected', got '$line'" >&2
        exit 1
    fi
    echo "PASS $name: $line"
}

run_case default "identity=1 use_memory_base=0 mask=0x0 window_bits=0"
run_case explicit_bias "identity=0 use_memory_base=1 mask=0xffffffff window_bits=32" \
    SVM_MEM_IDENTITY=0
run_case explicit_identity "identity=1 use_memory_base=0 mask=0x0 window_bits=0" \
    SVM_MEM_IDENTITY=1
run_case explicit_window "identity=0 use_memory_base=1 mask=0xffffffff window_bits=32" \
    SVM_GUEST_BITS=32

collision_out="$(env SVM_MEM_MODE_TRACE=1 SVM_MEM_IDENTITY_TEST_COLLISION=1 \
    "$SVM" "$GUEST" 2>&1)"
collision_rc=$?
if [ "$collision_rc" -ne "$EXPECT_RC" ] ||
   ! printf '%s\n' "$collision_out" | grep -q 'falling back to the 32-bit bounded bias window' ||
   ! printf '%s\n' "$collision_out" | grep -q '^\[svm-mem-mode\] identity=0 use_memory_base=1 mask=0xffffffff window_bits=32'; then
    echo "FAIL collision_fallback" >&2
    echo "$collision_out" >&2
    exit 1
fi
echo "PASS collision_fallback: warning + bounded bias + guest rc=$EXPECT_RC"
