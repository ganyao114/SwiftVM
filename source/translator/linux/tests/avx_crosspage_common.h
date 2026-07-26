//
// avx_crosspage_common.h -- the 32-byte-access page-boundary experiment shared
// by the SwiftVM guest (avx_crosspage_x86_64.c) and the native oracle
// (avx_crosspage_host.c).
//
// WHAT IS UNDER TEST.  SwiftVM's contract C1 lowers a 256-bit memory access to
// two 16-byte halves.  On x86 the same access is one operation, so when it
// straddles a page boundary whose second page is absent the whole access
// faults; a split lowering can instead commit the first half and then fault,
// leaving a torn write.  (Note: x86 does NOT promise atomicity for accesses
// that cross a page boundary, so a partial write is not by itself an
// architectural violation -- what matters is that a *restartable* fault must
// leave state from which re-execution produces the correct result, and that
// the guest-visible outcome matches what the hardware would produce.)
//
// STAGES.  The interesting cases are fatal by construction, so the program
// takes a stage number and does exactly one thing per run:
//
//   0  control: straddling 32-byte store AND load with both pages mapped.
//      Fully checksummed, compared bit-for-bit against the native oracle.
//      This is the stage that must MATCH; it is also the stage that proves
//      the split lowering gets the non-faulting case right.
//   1  straddling store, second page unmapped        -> must fault
//   2  straddling load,  second page unmapped        -> must fault
//   3  store entirely inside the hole                -> must fault (control
//      for 1 and 2: isolates "straddling" from "faults at all")
//   4  straddling store one byte before the boundary so that only the LAST
//      byte is in the hole                           -> must fault
//   5  observer: run stage 1 somewhere survivable and report whether the
//      first page was partially written.  Natively that is a SIGSEGV handler;
//      in the guest it is a clone() worker, because SwiftVM delivers no
//      signals to guests.
//
// Layout, in units of 16 KiB so the boundary is a real page boundary on
// x86-64 Linux (4 KiB), macOS/Rosetta (4 KiB) and the arm64 host that backs
// SwiftVM's guest memory (16 KiB) alike:
//
//     [ unit 0: mapped ][ unit 1: UNMAPPED ][ unit 2: mapped ]
//
#ifndef SVM_AVX_CROSSPAGE_COMMON_H
#define SVM_AVX_CROSSPAGE_COMMON_H

#include <immintrin.h>

typedef unsigned char u8;
typedef unsigned long long u64;

#define XP_UNIT 16384UL
#define XP_FILL 0x5A

// Supplied by the shim.
void svm_write(const char *buf, unsigned long len);
void svm_exit(int code);
void *xp_mmap(unsigned long len);
int xp_munmap(void *addr, unsigned long len);
// Stage 5 only: run fn(arg) somewhere a fault is survivable.  Returns 1 if it
// faulted, 0 if it completed, -1 if the mechanism itself is unavailable.
int xp_run_faulting(void (*fn)(void *), void *arg);

static void emit_kv(const char *name, u64 value) {
    char buf[64];
    unsigned long i = 0;
    while (name[i]) { buf[i] = name[i]; i++; }
    buf[i++] = '=';
    for (int shift = 60; shift >= 0; shift -= 4)
        buf[i++] = "0123456789abcdef"[(value >> shift) & 0xF];
    buf[i++] = '\n';
    svm_write(buf, i);
}

static u64 xp_mix(u64 h, u64 v) {
    h ^= v;
    h *= 0x100000001b3ULL;
    h ^= h >> 29;
    return h;
}

static u64 xp_load_sink[4];

static void xp_do_store(void *p) {
    _mm256_storeu_si256((__m256i *)p, _mm256_set1_epi8((char)0xA5));
}

// The empty asm makes the loaded value opaque, which is load-bearing: without
// it clang notices that only lane 0 is ever inspected and narrows the 32-byte
// load to an 8-byte one, at which point the access no longer straddles
// anything and the whole experiment silently measures nothing.
static void xp_do_load(void *p) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __asm__ volatile("" : "+x"(v));
    _mm256_storeu_si256((__m256i *)xp_load_sink, v);
}

static int xp_changed(const u8 *p, unsigned long n) {
    int changed = 0;
    for (unsigned long i = 0; i < n; i++)
        if (p[i] != XP_FILL) changed = 1;
    return changed;
}

static int avx_crosspage_run(int stage) {
    // Over-allocate so the working region can be aligned to XP_UNIT.
    u8 *raw = (u8 *)xp_mmap(4 * XP_UNIT);
    if (!raw) { emit_kv("mmap_failed", 1); return 90; }
    u8 *base = (u8 *)(((unsigned long)raw + XP_UNIT - 1) & ~(XP_UNIT - 1));
    u8 *hole = base + XP_UNIT;

    if (stage == 0) {
        // Straddling store then straddling load, both pages present.
        for (int i = 0; i < 128; i++) hole[-64 + i] = (u8)(i * 7 + 1);
        u64 h = 0xcbf29ce484222325ULL;
        for (int off = -20; off <= 20; off += 4) {
            xp_do_store(hole + off);
            for (int i = -64; i < 64; i++) h = xp_mix(h, hole[i]);
            for (int i = 0; i < 128; i++) hole[-64 + i] = (u8)(i * 11 + 3);
            xp_do_load(hole + off);
            for (int i = 0; i < 4; i++) h = xp_mix(h, xp_load_sink[i]);
        }
        emit_kv("straddle_mapped", h);
        return 0;
    }

    for (int i = 0; i < 64; i++) hole[-64 + i] = XP_FILL;
    if (xp_munmap(hole, XP_UNIT) != 0) { emit_kv("munmap_failed", 1); return 91; }
    // Progress marker: distinguishes "died setting the experiment up" from
    // "died taking the fault the experiment is about".
    emit_kv("armed", 1);

    switch (stage) {
        case 1:  // 16 bytes in the mapped page, 16 in the hole
            emit_kv("about_to_fault", 1);
            xp_do_store(hole - 16);
            break;
        case 2:
            emit_kv("about_to_fault", 2);
            xp_do_load(hole - 16);
            emit_kv("load_low_half", xp_load_sink[0]);
            emit_kv("load_high_half", xp_load_sink[3]);
            break;
        case 3:  // entirely inside the hole
            emit_kv("about_to_fault", 3);
            xp_do_store(hole + 1024);
            break;
        case 4:  // 31 bytes mapped, only the final byte in the hole
            emit_kv("about_to_fault", 4);
            xp_do_store(hole - 31);
            break;
        case 5: {
            int fault = xp_run_faulting(xp_do_store, hole - 16);
            emit_kv("observer_fault", (u64)(long)fault);
            emit_kv("first_page_partial", (u64)xp_changed(hole - 16, 16));
            emit_kv("first_page_prefix_intact", (u64)!xp_changed(hole - 64, 48));
            return 0;
        }
        default:
            emit_kv("bad_stage", (u64)stage);
            return 92;
    }
    // Reaching here means the access did NOT fault: that is itself a result.
    emit_kv("survived_no_fault", 1);
    emit_kv("first_page_partial", (u64)xp_changed(hole - 64, 64));
    return 0;
}

#endif  // SVM_AVX_CROSSPAGE_COMMON_H
