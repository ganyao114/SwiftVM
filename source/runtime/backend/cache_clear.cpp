//
// Created by 甘尧 on 2024/5/9.
//

#include "cache_clear.h"

#if __APPLE__
#include <libkern/OSCacheControl.h>
#endif

namespace swift::runtime::backend {

void ClearICache(void *start, size_t size) {
#ifdef __APPLE__
    sys_icache_invalidate(reinterpret_cast<char *>(start), size);
#else
    __builtin___clear_cache(reinterpret_cast<char *>(start),
                            reinterpret_cast<char *>(start) + size);
#endif
}

void ClearDCache(void *start, size_t size) {
#ifdef __APPLE__
    sys_dcache_flush(reinterpret_cast<char *>(start), size);
#endif
}

}