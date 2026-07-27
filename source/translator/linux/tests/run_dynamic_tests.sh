#!/usr/bin/env bash
# run_dynamic_tests.sh -- real glibc PT_INTERP end-to-end regression suite.
#
#   run_dynamic_tests.sh <path-to-svm_translator_linux>
#
# The script builds nothing. It executes the checked-in Ubuntu glibc 2.38
# guest in the default, AVX+XSAVE, and eager-binding configurations, then
# verifies that omitting SVM_SYSROOT fails with a clean loader diagnostic.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SVM="${1:-}"
GUEST="$HERE/real_hello_dyn_x86_64"
SYSROOT="$HERE/sysroot"

if [ -z "$SVM" ] || [ ! -x "$SVM" ]; then
    echo "usage: $0 <path-to-svm_translator_linux>" >&2
    exit 2
fi

fail=0
pass=0

run_positive() {
    local name="$1"
    shift
    local out rc
    out="$("$@" 2>&1)"
    rc=$?
    if [ "$rc" -eq 42 ] && [ "$out" = "Hello, real glibc!" ]; then
        echo "PASS $name: output matched, exit 42"
        pass=$((pass + 1))
    else
        echo "FAIL $name: exit=$rc output=${out:-<empty>}"
        fail=$((fail + 1))
    fi
}

run_positive "default lazy binding" \
    env SVM_SYSROOT="$SYSROOT" "$SVM" "$GUEST"
run_positive "AVX+XSAVE lazy binding" \
    env SVM_SYSROOT="$SYSROOT" SVM_AVX=1 SVM_XSAVE=1 "$SVM" "$GUEST"
run_positive "eager binding" \
    env SVM_SYSROOT="$SYSROOT" LD_BIND_NOW=1 "$SVM" "$GUEST"

negative_out="$(env -u SVM_SYSROOT "$SVM" "$GUEST" 2>&1)"
negative_rc=$?
if [ "$negative_rc" -ne 0 ] &&
   echo "$negative_out" | grep -q "Guest interpreter" &&
   echo "$negative_out" | grep -q "SVM_SYSROOT"; then
    echo "PASS missing sysroot: clean interpreter diagnostic, exit $negative_rc"
    pass=$((pass + 1))
else
    echo "FAIL missing sysroot: exit=$negative_rc output=${negative_out:-<empty>}"
    fail=$((fail + 1))
fi

echo "dynamic tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
