#!/usr/bin/env bash
# rt_sigreturn stale-private-frame/TLS regression.
#
#   run_sigreturn_stale_frame_test.sh <path-to-svm_translator_linux>
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SVM="${1:-}"
GUEST="$HERE/sigreturn_stale_frame_x86_64"

if [ -z "$SVM" ] || [ ! -x "$SVM" ] || [ ! -x "$GUEST" ]; then
    echo "usage: $0 <path-to-svm_translator_linux>" >&2
    exit 2
fi

output="$("$SVM" "$GUEST" 2>&1)"
status=$?
if [ "$status" -eq 0 ] &&
   printf '%s\n' "$output" | grep -q '^PASS sigreturn TLS hits='; then
    echo "$output"
    exit 0
fi

echo "FAIL sigreturn stale-frame regression: exit=$status" >&2
printf '%s\n' "$output" >&2
exit 1
