//
// bench_suite_native.c -- native x86-64 oracle for the SwiftVM benchmark guest.
//
// Compiled for real x86-64 and executed (under Rosetta on Apple Silicon) to
// produce bench_suite_x86_64.native.txt.  It includes bench_suite_kernels.h
// verbatim -- the same text the guest compiles -- so the two sides cannot
// drift.  See build_bench_tests.sh.
//
// This exists only to record golden checksums.  It is NOT a performance
// reference: Rosetta is itself a translator, so its wall time says nothing
// about SwiftVM.
//
#include <stdio.h>
#include <string.h>

typedef unsigned long ulong_t;
typedef unsigned long long u64_t;
typedef unsigned int u32_t;

static void bench_put_str(const char *s) { fputs(s, stdout); }
static void bench_put_hex64(u64_t v) { printf("0x%016llx\n", v); }

#include "bench_suite_kernels.h"

int main(int argc, char **argv) { return bench_run(argc, argv); }
