#!/system/bin/sh
# One benchmark observation. Scheduling, deployment and FEX service ownership are
# deliberately handled by the caller; this script never stops another process.
set -eu
umask 022

die() { printf '%s\n' "$*" >&2; exit 64; }
[ "$#" -ge 5 ] || die 'usage: pmu-point.sh NEW_SAMPLE_DIR SIMPLEPERF CRC_OR_sha256:DIGEST LAUNCHER GUEST [ARG...]'
sample=$1
pmu=$2
expected=$3
crc=
stdout_digest=
case "$expected" in
    0x4983|0xd340) crc=$expected ;;
    sha256:*)
        stdout_digest=${expected#sha256:}
        [ "${#stdout_digest}" -eq 64 ] || die 'expected stdout SHA-256 must have 64 hex digits'
        case "$stdout_digest" in *[!0-9a-f]*) die 'expected stdout SHA-256 must be lowercase hex' ;; esac
        ;;
    *) die 'expected a CoreMark CRC or sha256:DIGEST for exact benchmark stdout' ;;
esac
shift 3
launcher=$1
guest=$2
[ "$(id -u)" = 0 ] || die 'root is required'
[ "$(getprop ro.product.model)" = NX729J ] || die 'expected NX729J'
[ "$(cat /sys/devices/system/cpu/cpu4/online)" = 1 ] || die 'CPU 4 is offline'
case "$sample" in
    /data/local/tmp/svm-middle-pmu-*/samples/*) ;;
    *) die 'use a private svm-middle-pmu-* sample directory' ;;
esac
case "$sample" in *'/../'*|*'/./'*) die 'sample path must not contain dot components' ;; esac
[ -x "$pmu" ] && [ -x "$1" ] && [ -r "$2" ] || die 'missing profiler, launcher or guest'
[ "$(cat /sys/class/thermal/thermal_zone31/type)" = cpuss-0 ] || die 'thermal zone 31 changed'
[ "$(cat /sys/class/thermal/thermal_zone35/type)" = cpu-1-0 ] || die 'thermal zone 35 changed'
[ "$(cat /sys/class/thermal/thermal_zone41/type)" = cpu-1-6 ] || die 'thermal zone 41 changed'
# mkdir without -p refuses an existing observation, including a failed one.
mkdir "$sample"

read_value() {
    printf '%s=' "$1"
    if [ -r "$2" ]; then cat "$2"; else printf 'unavailable\n'; fi
}
snapshot() {
    printf 'utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    read_value uptime /proc/uptime
    printf 'model=%s\nfingerprint=%s\n' "$(getprop ro.product.model)" "$(getprop ro.build.fingerprint)"
    read_value cpu4_online /sys/devices/system/cpu/cpu4/online
    for item in scaling_governor scaling_cur_freq scaling_min_freq scaling_max_freq; do
        read_value "$item" "/sys/devices/system/cpu/cpu4/cpufreq/$item"
    done
    for zone in 31 35 41; do
        read_value "thermal_${zone}_type" "/sys/class/thermal/thermal_zone$zone/type"
        read_value "thermal_${zone}_temp" "/sys/class/thermal/thermal_zone$zone/temp"
    done
    for item in status temp capacity; do
        read_value "battery_$item" "/sys/class/power_supply/battery/$item"
    done
    read_value usb_online /sys/class/power_supply/usb/online
}

printf '%s\000' "$@" > "$sample/argv.nul"
env | /system/bin/grep -E '^(SVM_|FEX_|XDG_RUNTIME_DIR=|PATH=|LD_LIBRARY_PATH=)' | sort > "$sample/environment.txt"
if [ -n "$stdout_digest" ]; then
    printf '%s\n' "$stdout_digest" > "$sample/expected-stdout-sha256.txt"
else
    printf '%s\n' "$crc" > "$sample/expected-crc.txt"
fi
sha256sum "$0" "$pmu" "$launcher" "$guest" > "$sample/identity-before.txt"
snapshot > "$sample/device-before.txt"

# Pin simpleperf before it creates the measured task. --cpu filters counters;
# putting taskset only inside the measured command can miss its initial work.
# simpleperf can return zero when its child dies from a signal. A parent shell
# waits for the actual launcher and records that status independently. Both
# shells and the PID/affinity query are included equally in all three arms.
child_wrapper='printf "SVM_BENCH_PID=%s\n" "$$" >&2; /system/bin/taskset -p $$ >&2; exec "$@"'
wrapper='sample=$1; child_wrapper=$2; shift 2; printf "SVM_BENCH_COUNTER_PID=%s\n" "$$" >&2; /system/bin/sh -c "$child_wrapper" pmu-target "$@" & target=$!; printf "SVM_BENCH_CHILD_PID=%s\n" "$target" >&2; if wait "$target"; then status=0; else status=$?; fi; printf "%s\n" "$status" > "$sample/target-exit-code.txt"; exit "$status"'
set -- /system/bin/taskset 10 "$pmu" stat --csv --verbose --cpu 4 \
    --group cpu-cycles,instructions -o "$sample/pmu.csv" \
    /system/bin/sh -c "$wrapper" pmu-observation "$sample" "$child_wrapper" "$@"
printf '%s\000' "$@" > "$sample/profiler-argv.nul"
if "$@" > "$sample/stdout.txt" 2> "$sample/stderr.txt"; then status=0; else status=$?; fi
printf '%s\n' "$status" > "$sample/profiler-exit-code.txt"
snapshot > "$sample/device-after.txt"
sha256sum "$0" "$pmu" "$launcher" "$guest" > "$sample/identity-after.txt"
[ "$status" -eq 0 ] || exit "$status"
target_status=$(cat "$sample/target-exit-code.txt")
[ "$target_status" -eq 0 ] || exit "$target_status"
if [ -n "$stdout_digest" ]; then
    actual=$(sha256sum "$sample/stdout.txt")
    [ "${actual%% *}" = "$stdout_digest" ] || die 'benchmark stdout mismatch; retain this observation'
else
    actual=$(sed -n 's/.*\[0\]crcfinal[[:space:]]*:[[:space:]]*\(0x[0-9a-fA-F]*\)[[:space:]]*$/\1/p' "$sample/stdout.txt")
    [ "$actual" = "$crc" ] || die 'CoreMark final CRC mismatch; retain this observation'
fi
