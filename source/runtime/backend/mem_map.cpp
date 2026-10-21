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
#include "base/scope_exit.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#endif  // ^^^ Linux ^^^

#include "cache_clear.h"
#include "mem_map.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <thread>

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
    explicit Impl(u32 size, bool exec) : executable(exec), map_size(size) {}

    void Init() {
        const u32 size = map_size;
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
                    mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            if (backing_memory == MAP_FAILED) {
                backing_memory = nullptr;
                throw std::bad_alloc{};
            }
            RecordView(backing_memory, size);
            construction_complete = true;
            return;
        }
#elif ANDROID || defined(__linux__)
        fd = memfd_create("HostMemory", 0);
#endif
        if (fd == -1) {
            throw std::bad_alloc{};
        } else {
            int ret = ftruncate(fd, map_size);
            if (ret != 0) {
                throw std::bad_alloc{};
            }
        }

        backing_memory = reinterpret_cast<u8*>(Map(size, 0, Mode::ReadWrite, false));
        construction_complete = true;
    }

    [[nodiscard]] void* Map(u32 size, u32 offset, MemMap::Mode mode, bool pri) {
        if (release_started) {
            throw std::logic_error("mapping owner has begun release");
        }
        if (size == 0 || offset > map_size || size > map_size - offset) {
            throw std::bad_alloc{};
        }
        // Metadata allocation must finish before acquiring a kernel view.
        views.reserve(views.size() + 1);
        auto flags = GetProtectFlags(mode);
#if __APPLE__
        if (fd < 0) {
            auto res = MirrorMemory(offset + backing_memory, size, pri);
            if (!res) throw std::bad_alloc{};
            // The view ledger owns every successful acquisition before a
            // later protection failure can unwind through constructor guards.
            RecordView(res, size);
            if (mprotect(res, size, flags) != 0) throw std::bad_alloc{};
            return res;
        }
#endif
        auto res = mmap(nullptr, size, flags, pri ? MAP_PRIVATE : MAP_SHARED, fd, offset);
        if (res == MAP_FAILED) {
            throw std::bad_alloc{};
        }
        RecordView(res, size);
        return res;
    }

    void Free(u32 host_offset, u32 length) const {
        if (release_started || !backing_memory) return;
#ifdef __APPLE__
        madvise(backing_memory + host_offset, length, MADV_DONTNEED);
#else
        madvise(backing_memory + host_offset, length, MADV_REMOVE);
#endif
    }

    [[nodiscard]] MemMap::UnmapResult UnmapChecked(void* mem, u32 size) noexcept {
        const auto view = std::find_if(views.begin(), views.end(), [&](const View& item) {
            return item.address == mem && item.size == size;
        });
        if (view == views.end()) return {.error = EINVAL};
        const int saved_errno = errno;
        const int result = munmap(mem, size);
        const int error = result == 0 ? 0 : errno;
        errno = saved_errno;
        if (error != 0) return {.error = error};
        if (mem == backing_memory) backing_memory = nullptr;
        views.erase(view);
        return {};
    }

    [[nodiscard]] u8* GetBackend() const { return backing_memory; }

    [[nodiscard]] bool NeedsDestructorRelease() const noexcept {
        return !executable || !construction_complete || release_started;
    }

    // Impl destruction only frees metadata. Its live kernel resources must
    // already have been released or transferred to the retained-owner queue.
    ~Impl() = default;

    [[nodiscard]] MemMap::ReleaseResult ReleaseAfterQuiescence() noexcept {
        release_started = true;
        unmap_error = 0;
        // The initial RW backing is the first view; remove later aliases first.
        while (!views.empty()) {
            const auto view = views.back();
            const auto result = UnmapChecked(view.address, view.size);
            if (!result.Released()) {
                unmap_error = result.error;
                return GetReleaseState();
            }
        }
        CloseDescriptor();
        return GetReleaseState();
    }

    [[nodiscard]] MemMap::ReleaseResult GetReleaseState() const noexcept {
        return {.remaining_views = static_cast<u32>(views.size()),
                .backing_mapped = backing_memory != nullptr,
                .descriptor_open = fd >= 0,
                .unmap_error = unmap_error,
                .close_error = close_error};
    }

    static void Retain(std::unique_ptr<Impl>& owner) noexcept {
        auto* item = owner.release();
        RetainedGuard guard(retained_lock);
        item->retained_next = retained_head;
        retained_head = item;
        ++retained_count;
    }

    [[nodiscard]] static MemMap::RetainedReleaseResult RetainedState() noexcept {
        RetainedGuard guard(retained_lock);
        return {.owners = retained_count,
                .first_failure = retained_head ? retained_head->GetReleaseState()
                                               : MemMap::ReleaseResult{}};
    }

    [[nodiscard]] static MemMap::RetainedReleaseResult RetryRetained() noexcept {
        RetainedGuard retry_guard(retry_lock);
        Impl* batch;
        {
            RetainedGuard guard(retained_lock);
            batch = retained_head;
            retained_head = nullptr;
        }
        // Each owner present at entry is retried once. New and still-failing
        // owners remain queued; syscall and delete callbacks run without the
        // queue lock, and in-flight owners remain counted until completion.
        while (batch) {
            auto* item = batch;
            batch = item->retained_next;
            item->retained_next = nullptr;
            const bool complete = item->ReleaseAfterQuiescence().Complete();
            {
                RetainedGuard guard(retained_lock);
                if (complete) {
                    --retained_count;
                } else {
                    item->retained_next = retained_head;
                    retained_head = item;
                }
            }
            if (complete) delete item;
        }
        return RetainedState();
    }

    void CloseUnretiredDescriptor() noexcept { CloseDescriptor(); }

private:
    class RetainedGuard {
    public:
        explicit RetainedGuard(std::atomic_flag& flag) noexcept : flag(flag) {
            while (flag.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
        }
        ~RetainedGuard() { flag.clear(std::memory_order_release); }
    private:
        std::atomic_flag& flag;
    };
    // No allocating singleton or static destructor: late static owners can
    // still transfer failures, and process exit never masquerades as cleanup.
    inline static constinit std::atomic_flag retained_lock = ATOMIC_FLAG_INIT;
    inline static constinit std::atomic_flag retry_lock = ATOMIC_FLAG_INIT;
    inline static Impl* retained_head{};
    inline static size_t retained_count{};
    Impl* retained_next{};
    struct View {
        void* address{};
        u32 size{};
    };

    void RecordView(void* address, u32 size) noexcept {
        views.push_back({address, size});
    }

    void CloseDescriptor() noexcept {
        const int saved_errno = errno;
        if (fd >= 0) {
            // Linux consumes the descriptor even when close reports an error.
            // Other platforms can be ambiguous; report the error without
            // risking a second close of a descriptor reused by another owner.
            const int descriptor = fd;
            fd = -1;
            if (close(descriptor) != 0) close_error = errno;
        }
        errno = saved_errno;
    }

    const bool executable;
    bool construction_complete{false};
    bool release_started{false};
    int fd{-1};
    int unmap_error{};
    int close_error{};
    u8* backing_memory{};
    u32 map_size;
    StackVector<View, 2> views;
};

#endif  // ^^^ Unix ^^^

MemMap::MemMap(u32 size, bool exec) : arena_size(size) {
    impl = std::make_unique<Impl>(size, exec);
    try {
        impl->Init();
    } catch (...) {
        if (!impl->ReleaseAfterQuiescence().Complete()) Impl::Retain(impl);
        throw;
    }
}

MemMap::~MemMap() {
    if (!impl) return;
    if (impl->NeedsDestructorRelease()) {
        // Preserve the existing single destructor retry for transient rollback
        // failures. A persistent failure still transfers the same live Impl;
        // the retry is bounded and a consumed close descriptor is never reused.
        const auto state = impl->ReleaseAfterQuiescence();
        if (!state.Complete()) Impl::Retain(impl);
    } else {
        // Upper-level retirement is still required for a published arena.
        impl->CloseUnretiredDescriptor();
    }
}

void* MemMap::Map(u32 size, u32 offset, MemMap::Mode mode, bool pri) {
    return impl->Map(size, offset, mode, pri);
}

void MemMap::Protect(u32 size, u32 offset, MemMap::Mode mode) {}

void MemMap::Unmap(void* mem, swift::u32 size) { (void)impl->UnmapChecked(mem, size); }

MemMap::UnmapResult MemMap::UnmapChecked(void* mem, u32 size) noexcept {
    return impl->UnmapChecked(mem, size);
}

MemMap::ReleaseResult MemMap::ReleaseAfterQuiescence() noexcept {
    return impl->ReleaseAfterQuiescence();
}

MemMap::ReleaseResult MemMap::GetReleaseState() const noexcept {
    return impl->GetReleaseState();
}

MemMap::RetainedReleaseResult MemMap::GetRetainedReleaseState() noexcept {
    return Impl::RetainedState();
}

MemMap::RetainedReleaseResult MemMap::RetryRetainedReleases() noexcept {
    return Impl::RetryRetained();
}

void MemMap::Free(u32 offset, u32 size) { impl->Free(offset, size); }

u8* MemMap::GetMemory() { return impl->GetBackend(); }

u32 MemMap::GetSize() const { return arena_size; }

}  // namespace swift::runtime::backend
