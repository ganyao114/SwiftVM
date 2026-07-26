#!/bin/bash
#
# Guest/host address-space isolation regression suite.
#
# WHAT IS UNDER TEST
#   A guest memory access is `host = guest + bias`.  Without a bound on the
#   guest address, a large enough guest address covers the distance from the
#   guest reservation to any host mapping, and the access simply succeeds:
#     * READ  — a hand-driven guest read 0xfeedfacf out of the translator's own
#               Mach-O header;
#     * WRITE — a hand-driven guest planted 0x4141414141414141 in a host
#               malloc() buffer, observed from a host breakpoint.
#   Both were done with lldb and ASLR off, because the host addresses move per
#   run.  This suite asserts the same property WITHOUT needing a host address:
#   with a bounded guest window every guest address truncates into the window,
#   so `base` and `base + 2^k` (k >= window bits) must alias.  If they do not,
#   the access left the window — which is exactly "it reached host memory".
#
#   The suite therefore fails on the unbounded build (SVM_GUEST_BITS=0, kept as
#   the diagnostic escape hatch) and passes on the bounded one.  Run it both
#   ways to see the regression it locks down:
#       SVM_ISOLATION_EXPECT=broken run_isolation_tests.sh   # unbounded
#
# ASSERTIONS
#   alias/load : guest exit code 0 (the aliased access matched).
#   wild       : the HOST must survive.  The guest may halt (PageFatal) with
#                any exit code; a host killed by a signal (rc >= 128) or an
#                abort is a failure.  Nothing is asserted about guest results.
#
set -u

SVM="${1:-}"
if [ -z "$SVM" ]; then
    echo "usage: $0 <path-to-svm_translator_linux>" >&2
    exit 2
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
GEN="$HERE/gen_isolation_guest_x86_64.py"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# SVM_GUEST_BITS=0 reproduces the pre-fix unbounded behaviour.
BROKEN="${SVM_ISOLATION_EXPECT:-fixed}"
ENVPFX=""
[ "$BROKEN" = "broken" ] && ENVPFX="SVM_GUEST_BITS=0"

# Aliasing only holds for deltas that are whole multiples of the window size,
# so the deltas are derived from the window under test rather than hard-coded.
# (For the unbounded run the 32-bit default is used: those deltas are exactly
# the ones that escaped.)
BITS="${SVM_GUEST_BITS:-32}"
[ "$BITS" = "0" ] && BITS=32
DELTAS="$(python3 -c "
b=$BITS
w=1<<b
for d in (w, 2*w, 8*w, 1<<47, 1<<63, (-w)&((1<<64)-1)):
    print('%x' % (d & ((1<<64)-1)))
")"

pass=0
fail=0
report() {  # report <name> <ok|bad> <detail>
    if [ "$2" = "ok" ]; then
        pass=$((pass + 1))
        printf 'PASS  %-28s %s\n' "$1" "$3"
    else
        fail=$((fail + 1))
        printf 'FAIL  %-28s %s\n' "$1" "$3"
    fi
}

run_guest() {  # run_guest <elf> -> sets RC
    if [ -n "$ENVPFX" ]; then
        env $ENVPFX "$SVM" "$1" >/dev/null 2>&1
    else
        "$SVM" "$1" >/dev/null 2>&1
    fi
    RC=$?
}

# --- 1. write side: a store through base+delta must stay in the window ------
# delta >= 2^32 (the default window).  2^63 also covers signed wraparound.
for D in $DELTAS; do
    elf="$WORK/alias_$D.elf"
    python3 "$GEN" alias "$D" -o "$elf" || { report "alias_$D" bad "generator failed"; continue; }
    run_guest "$elf"
    if [ "$RC" -ge 128 ]; then
        report "alias_$D" bad "HOST killed by signal (rc=$RC)"
    elif [ "$RC" -eq 0 ]; then
        report "alias_$D" ok "store aliased into the window"
    else
        report "alias_$D" bad "store escaped the guest window (rc=$RC)"
    fi
done

# --- 2. read side: a load through base+delta must stay in the window --------
for D in $DELTAS; do
    elf="$WORK/load_$D.elf"
    python3 "$GEN" load "$D" -o "$elf" || { report "load_$D" bad "generator failed"; continue; }
    run_guest "$elf"
    if [ "$RC" -ge 128 ]; then
        report "load_$D" bad "HOST killed by signal (rc=$RC)"
    elif [ "$RC" -eq 0 ]; then
        report "load_$D" ok "load aliased into the window"
    else
        report "load_$D" bad "load escaped the guest window (rc=$RC)"
    fi
done

# --- 3. wild pointers: the host must survive, whatever the guest does -------
for A in ffffffffffffffff fffffffffffff000 8000000000000000 7fffffffffffffff \
         0000800000000000 dead0000beef0000 0000000000000000 fffffffffffffffc; do
    elf="$WORK/wild_$A.elf"
    python3 "$GEN" wild "$A" -o "$elf" || { report "wild_$A" bad "generator failed"; continue; }
    run_guest "$elf"
    if [ "$RC" -ge 128 ]; then
        report "wild_$A" bad "HOST killed by signal (rc=$RC)"
    else
        report "wild_$A" ok "host survived (guest rc=$RC)"
    fi
done

echo "----"
echo "isolation: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
