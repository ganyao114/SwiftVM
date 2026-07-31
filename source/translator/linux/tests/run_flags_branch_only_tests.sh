#!/usr/bin/env bash
set -u

here="$(cd "$(dirname "$0")" && pwd)"
svm="${1:-}"
guest="$here/flags_branch_only_x86_64"
source="$here/flags_branch_only_x86_64.S"

if [ -z "$svm" ] || [ ! -x "$svm" ]; then
    echo "usage: $0 <svm_translator_linux>"
    exit 2
fi

orb -m ubuntu-x64 bash -lc \
    "gcc -nostdlib -static -Wl,--build-id=none -o '$guest' '$source'" ||
    exit 2

native="$(orb -m ubuntu-x64 "$guest")"
native_rc=$?
if [ "$native_rc" -ne 0 ]; then
    echo "native FAIL rc=$native_rc output=$native"
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

run_case() {
    local name="$1"
    shift
    env SVM_JIT_CACHE= "$@" "$svm" "$guest" \
        >"$tmp/$name.out" 2>"$tmp/$name.err"
    local rc=$?
    local output
    output="$(cat "$tmp/$name.out")"
    if [ "$rc" -ne 0 ] || [ "$output" != "$native" ]; then
        echo "$name FAIL rc=$rc output=$output"
        return 1
    fi
    echo "$name OK rc=$rc output=$output"
}

run_case off_func SVM_FUNC_BASE=1 SVM_FLAGS_BRANCH_ONLY=0 || exit 1
run_case on_func SVM_FUNC_BASE=1 SVM_FLAGS_BRANCH_ONLY=1 || exit 1
run_case off_block SVM_FUNC_BASE=0 SVM_FLAGS_BRANCH_ONLY=0 || exit 1
run_case on_block SVM_FUNC_BASE=0 SVM_FLAGS_BRANCH_ONLY=1 || exit 1
run_case off_interp SVM_ENABLE_JIT=0 SVM_FLAGS_BRANCH_ONLY=0 || exit 1
run_case on_interp SVM_ENABLE_JIT=0 SVM_FLAGS_BRANCH_ONLY=1 || exit 1

echo "directed matrix: PASS"
