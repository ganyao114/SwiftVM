//
// Created by 甘尧 on 2023/9/27.
//
#ifdef _WIN32

#include <iterator>
#include <unordered_map>
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>

#elif defined(__linux__) || defined(__FreeBSD__) ||                                                \
        defined(__APPLE__)  // ^^^ Windows ^^^ vvv Linux vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/common/scope_exit.h"

#endif  // ^^^ Linux ^^^

#include "mem_map.h"

namespace swift::runtime::backend {

const size_t PageAlignment = getpagesize();
constexpr size_t HugePageSize = 0x200000;

#if __APPLE__
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

extern "C" kern_return_t mach_vm_remap(vm_map_t,
                                       mach_vm_address_t*,
                                       mach_vm_size_t,
                                       mach_vm_offset_t,
                                       int,
                                       vm_map_t,
                                       mach_vm_address_t,
                                       boolean_t,
                                       vm_prot_t*,
                                       vm_prot_t*,
                                       vm_inherit_t);

static int shm_unlink_or_close(const char* name, int fd) {
    int save;

    if (shm_unlink(name) == -1) {
        save = errno;
        close(fd);
        errno = save;
        return -1;
    }
    return fd;
}

static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-";
    struct timespec tv {};
    unsigned long r;
    char* const limit = name + sizeof(name) - 1;
    char* start;
    char* fill;
    int fd, tries;

    *limit = 0;
    start = name + strlen(name);
    for (tries = 0; tries < 4; tries++) {
        clock_gettime(CLOCK_REALTIME, &tv);
        r = (unsigned long)tv.tv_sec + (unsigned long)tv.tv_nsec;
        for (fill = start; fill < limit; r /= 8) *fill++ = '0' + (r % 8);
        fd = shm_open(name, flags, mode);
        if (fd != -1) return shm_unlink_or_close(name, fd);
        if (errno != EEXIST) break;
    }
    return -1;
}

void* MirrorMemory(void* base, size_t size, bool copy) {
    kern_return_t ret;
    mach_vm_address_t mirror;
    vm_prot_t cur_prot, max_prot;

    mirror = 0;
    ret = mach_vm_remap(mach_task_self(),
                        &mirror,
                        size,
                        0,
                        VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR,
                        mach_task_self(),
                        (mach_vm_address_t)base,
                        copy,
                        &cur_prot,
                        &max_prot,
                        VM_INHERIT_DEFAULT);
    if (ret != KERN_SUCCESS) {
        return nullptr;
    }
    return reinterpret_cast<void*>(mirror);
}

#elif ANDROID
#include <syscall.h>
#include <unistd.h>

static inline int memfd_create(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}

#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif

#ifndef F_ADD_SEALS
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10)

#define F_SEAL_SEAL 0x0001   /* prevent further seals from being set */
#define F_SEAL_SHRINK 0x0002 /* prevent file from shrinking */
#define F_SEAL_GROW 0x0004   /* prevent file from growing */
#define F_SEAL_WRITE 0x0008  /* prevent writes */
#endif
#endif

#ifdef _WIN32

#elif defined(__linux__) || defined(__FreeBSD__) ||                                                \
        defined(__APPLE__)  // ^^^ Windows ^^^ vvv Linux vvv

static int GetProtectFlags(MemMap::Mode mode) {
    int result{PROT_NONE};
    if (mode & MemMap::Read) {
        result |= PROT_READ;
    }
    if (mode & MemMap::Write) {
        result |= PROT_WRITE;
    }
    if (mode & MemMap::Executable) {
        result |= PROT_EXEC;
    }
    return result;
}

class MemMap::Impl {
public:
    explicit Impl(u32 size, bool exec) : map_size(size), executable(exec) {
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size != PageAlignment) {
            throw std::bad_alloc{};
        }

        // Backing memory initialization
#if defined(__FreeBSD__) && __FreeBSD__ < 13
        // XXX Drop after FreeBSD 12.* reaches EOL on 2024-06-30
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#elif __APPLE__
        if (!executable) {
            fd = shm_open_anon(O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        } else {
            backing_memory = reinterpret_cast<u8*>(
                    mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0));
        }
#elif ANDROID
        fd = memfd_create("HostMemory", 0);
#endif
        if (fd == -1) {
            throw std::bad_alloc{};
        } else if (fd != 0) {
            int ret = ftruncate(fd, map_size);
            if (ret != 0) {
                throw std::bad_alloc{};
            }
        }

        if (!backing_memory) {
            backing_memory = reinterpret_cast<u8*>(Map(size, 0, Mode::ReadWrite, false));
        }
    }

    void* Map(u32 size, u32 offset, MemMap::Mode mode, bool pri) {
        auto flags = GetProtectFlags(mode);
#if __APPLE__
        if (!fd) {
            auto res = MirrorMemory(offset + backing_memory, size, pri);
            ASSERT(res);
            ASSERT(mprotect(res, size, flags) == 0);
            return res;
        }
#endif
        auto res = mmap(nullptr, size, flags, pri ? MAP_PRIVATE : MAP_SHARED, fd, offset);
        if (res == MAP_FAILED) {
            throw std::bad_alloc{};
        }
        return res;
    }

    void Free(u32 host_offset, u32 length) const {
#ifdef __APPLE__
        madvise(backing_memory + host_offset, length, MADV_DONTNEED);
#else
        madvise(backing_memory + host_offset, length, MADV_REMOVE);
#endif
    }

    bool Unmap(void* mem, u32 size) { return munmap(mem, size); }

    u8* GetBackend() const { return backing_memory; }

    virtual ~Impl() = default;

    const bool executable;
    int fd{};
    u8* backing_memory;
    u32 map_size;
};

#endif  // ^^^ Unix ^^^

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

MemMap::MemMap(u32 size, bool exec) : arena_size(size) {
    impl = std::make_unique<Impl>(size, exec);
}

MemMap::~MemMap() = default;

void* MemMap::Map(u32 size, u32 offset, MemMap::Mode mode, bool pri) {
    return impl->Map(size, offset, mode, pri);
}

void MemMap::Protect(u32 size, u32 offset, MemMap::Mode mode) {}

void MemMap::Unmap(void* mem, swift::u32 size) { impl->Unmap(mem, size); }

void MemMap::Free(u32 offset, u32 size) { impl->Free(offset, size); }

u8* MemMap::GetMemory() { return impl->GetBackend(); }

u32 MemMap::GetSize() const { return arena_size; }

}  // namespace swift::runtime::backend
