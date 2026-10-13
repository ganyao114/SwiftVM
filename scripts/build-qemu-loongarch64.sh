#!/bin/sh
# QEMU's original GPL-2.0-or-later notices remain in the patched source.
# Build a validation model, not a host-performance reference.
set -eu

if [ "$#" -gt 2 ]; then
    echo "usage: $0 [source-directory [build-directory]]" >&2
    exit 2
fi
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_source=${1:-"${SVM_QEMU_SOURCE:-$task_root/build/qemu-loongarch64-source}"}
task_build=${2:-"${SVM_QEMU_BUILD:-$task_root/build/qemu-loongarch64-build}"}
task_logs=${SVM_QEMU_LOG_DIR:-"$task_root/build/qemu-loongarch64-ibar"}
task_jobs=${SVM_QEMU_BUILD_JOBS:-${SVM_BUILD_JOBS:-3}}
task_revision=11aa0b1ff115b86160c4d37e7c37e6a6b13b77ea
task_patch="$task_root/scripts/qemu-loongarch64-ibar.patch"
case "$task_jobs" in ''|*[!0-9]*|0) echo "jobs must be a positive integer" >&2; exit 2;; esac
mkdir -p "$task_logs" "$task_build"
task_logs=$(CDPATH= cd -- "$task_logs" && pwd)
task_build=$(CDPATH= cd -- "$task_build" && pwd)

task_run() {
    task_label=$1
    shift
    printf '%s\n' "$task_label: started (log: $task_logs/$task_label.log)"
    if "$@" >"$task_logs/$task_label.log" 2>&1; then
        printf '%s\n' "$task_label: passed"
    else
        task_status=$?
        tail -n 30 "$task_logs/$task_label.log" >&2
        exit "$task_status"
    fi
}
if [ ! -d "$task_source/.git" ]; then
    task_run clone git clone --depth 1 --branch v8.2.2 \
        https://github.com/qemu/qemu.git "$task_source"
fi
task_source=$(CDPATH= cd -- "$task_source" && pwd)
if [ "$(git -C "$task_source" rev-parse HEAD)" != "$task_revision" ]; then
    echo "source must be QEMU v8.2.2 at $task_revision" >&2
    exit 2
fi
git -C "$task_source" diff --cached --quiet
git -C "$task_source" diff --quiet -- . \
    ':(exclude)target/loongarch/helper.h' \
    ':(exclude)target/loongarch/insn_trans/trans_memory.c.inc' \
    ':(exclude)target/loongarch/op_helper.c'
if git -C "$task_source" apply --check "$task_patch" >/dev/null 2>&1; then
    git -C "$task_source" apply "$task_patch"
elif git -C "$task_source" apply --reverse --check "$task_patch" >/dev/null 2>&1; then
    echo "IBAR model patch is already applied"
else
    echo "source has changes incompatible with the pinned IBAR patch" >&2
    exit 2
fi
git -C "$task_source" diff --check
git -C "$task_source" diff --no-ext-diff -- \
    target/loongarch/helper.h target/loongarch/insn_trans/trans_memory.c.inc \
    target/loongarch/op_helper.c >"$task_logs/applied.patch"
if ! cmp -s "$task_patch" "$task_logs/applied.patch"; then
    echo "patched source differs from the reviewed model patch" >&2
    exit 2
fi

# Ubuntu dependencies: pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev
# ninja-build python3-venv meson gcc g++. Dependencies are not installed here.
cd "$task_build"
task_run configure "$task_source/configure" --target-list=loongarch64-linux-user \
    --disable-system --disable-tools --disable-docs --disable-guest-agent \
    --disable-werror --disable-plugins --disable-capstone \
    --with-pkgversion=SwiftVM-IBAR-TBFlush-v1
task_run build ninja -j "$task_jobs" qemu-loongarch64
task_run version "$task_build/qemu-loongarch64" --version
{
    printf '%s\n' "Upstream: https://github.com/qemu/qemu.git" "Tag: v8.2.2" \
        "Commit: $task_revision" "Target: loongarch64-linux-user" "Jobs: $task_jobs" \
        "Model: conservative all-TB invalidation and CPU-loop exit on IBAR" \
        "Source: $task_source" "Build: $task_build"
    sha256sum "$task_patch" "$task_build/qemu-loongarch64"
    cat "$task_logs/version.log"
} >"$task_logs/manifest.txt"
cat "$task_logs/manifest.txt"

# Optional minimal reproduction, independent of SwiftVM. Supply a LoongArch
# Linux C compiler plus the guest runtime needed by its produced executable:
# SVM_QEMU_GUEST_CC=... SVM_QEMU_SYSROOT=... SVM_QEMU_RUNTIME_LIBDIR=... sh ...
# The same RX virtual address is reused 64 times; only its shared RW alias is
# written, then GCC's real __clear_cache/IBAR synchronizes both addresses.
if [ -n "${SVM_QEMU_GUEST_CC:-}" ]; then
    cat >"$task_build/dual-alias-ibar.c" <<'EOF'
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef __loongarch64
#error This reproduction must execute as a Linux LoongArch64 guest.
#endif
int main(void) {
    const long page = sysconf(_SC_PAGESIZE);
    const int fd = syscall(SYS_memfd_create, "qemu-dual-alias-ibar", 0);
    if (page <= 0 || fd < 0 || ftruncate(fd, page)) return 2;
    uint32_t *rw = mmap(0, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void *rx = mmap(0, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    close(fd);
    if (rw == MAP_FAILED || rx == MAP_FAILED) return 2;
    long (*entry)(void) = (long (*)(void))rx;
    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        const unsigned expected = cycle % 19 + 1;
        const uint32_t words[2] = {0x02c00004u | expected << 10, 0x4c000020u};
        memcpy(rw, words, sizeof(words)); /* addi.d a0,zero,expected; jirl zero,ra,0 */
        __builtin___clear_cache((char *)rw, (char *)rw + sizeof(words));
        __builtin___clear_cache((char *)rx, (char *)rx + sizeof(words));
        uint32_t visible[2];
        memcpy(visible, rx, sizeof(visible));
        const long actual = entry();
        if (memcmp(visible, words, sizeof(words)) || actual != expected) {
            fprintf(stderr, "FAIL cycle=%u expected=%u actual=%ld RXword=%08x RWword=%08x\n",
                    cycle, expected, actual, visible[0], rw[0]);
            return 1;
        }
    }
    puts("PASS dual RW/RX alias: 64 IBAR-synchronized rewrites at one RX address");
    return munmap(rw, page) || munmap(rx, page);
}
EOF
    task_run smoke-compile "$SVM_QEMU_GUEST_CC" -O2 \
        "$task_build/dual-alias-ibar.c" -o "$task_build/dual-alias-ibar"
    set --
    if [ -n "${SVM_QEMU_SYSROOT:-}" ]; then set -- "$@" -L "$SVM_QEMU_SYSROOT"; fi
    if [ -n "${SVM_QEMU_RUNTIME_LIBDIR:-}" ]; then
        set -- "$@" -E "LD_LIBRARY_PATH=$SVM_QEMU_RUNTIME_LIBDIR"
    fi
    if [ -n "${SVM_QEMU_BASELINE:-}" ]; then
        if "$SVM_QEMU_BASELINE" "$@" "$task_build/dual-alias-ibar" \
                >"$task_logs/smoke-baseline.log" 2>&1; then
            echo "baseline reproduction: passed"
        else
            task_status=$?
            echo "baseline reproduction: exit $task_status (see smoke-baseline.log)"
        fi
    fi
    task_run smoke "$task_build/qemu-loongarch64" "$@" "$task_build/dual-alias-ibar"
    cat "$task_logs/smoke.log"
fi
