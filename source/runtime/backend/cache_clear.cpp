//
// Created by 甘尧 on 2024/5/9.
//

#include "cache_clear.h"

#if __APPLE__
#include <libkern/OSCacheControl.h>
#elif defined(__linux__) && defined(__riscv)
#include <cstdint>
#include <sys/syscall.h>
#endif

namespace swift::runtime::backend {

#if defined(__linux__) && defined(__riscv)
namespace {
static_assert(__riscv_xlen == 64);
static_assert(SYS_riscv_flush_icache == 259 && SYS_exit_group == 94);
static_assert(sizeof(uintptr_t) == sizeof(long));

[[noreturn]] void FlushFailure() noexcept {
    register long status asm("a0") = 127;
    register long number asm("a7") = SYS_exit_group;
    asm volatile("ecall" : "+r"(status) : "r"(number) : "memory");
    __builtin_trap();
}

void FlushCache(void *start, size_t size) {
    const auto begin = reinterpret_cast<uintptr_t>(start);
    register uintptr_t result asm("a0") = begin;
    register uintptr_t end asm("a1") = begin + size;
    register uintptr_t flags asm("a2") = 0;
    register long number asm("a7") = SYS_riscv_flush_icache;
    // Linux preserves other registers; memory clobber orders emitted code stores.
    asm volatile("ecall" : "+r"(result) : "r"(end), "r"(flags), "r"(number) : "memory");
    if (result != 0) FlushFailure();
}
}
#endif

void ClearICache(void *start, size_t size) {
#ifdef __APPLE__
    sys_icache_invalidate(reinterpret_cast<char *>(start), size);
#elif defined(__linux__) && defined(__riscv)
    FlushCache(start, size);
#else
    __builtin___clear_cache(reinterpret_cast<char *>(start),
                            reinterpret_cast<char *>(start) + size);
#endif
}

void ClearDCache(void *start, size_t size) {
#ifdef __APPLE__
    sys_dcache_flush(reinterpret_cast<char *>(start), size);
#elif defined(__linux__) && defined(__riscv)
    FlushCache(start, size);
#else
    // On Linux/AArch64 __builtin___clear_cache lowers to the platform's
    // __clear_cache sequence, including D-cache clean-to-PoU. Calling it on
    // the RW alias here and ClearICache on the RX alias at the patch site is
    // intentionally conservative: the first call publishes dirty bytes from
    // the writable VA, the second invalidates instructions for the executable
    // VA. The previous empty implementation was insufficient for dual maps.
    __builtin___clear_cache(reinterpret_cast<char *>(start),
                            reinterpret_cast<char *>(start) + size);
#endif
}

}
