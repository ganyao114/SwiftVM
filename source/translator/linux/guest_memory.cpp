//
// Guest (ARM64 Linux) memory management — see guest_memory.h.
//

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
// Linux: MAP_FIXED_NOREPLACE (4.17+); never clobbers existing mappings.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif
#include "base/logging.h"
#include "guest_memory.h"
#include "runtime/common/svm_config.h"

namespace swift::linux {

bool GuestMemory::ReserveWindow(u32 bits) {
    ASSERT(bias_ == 0);       // the reservation installs the bias; call once
    ASSERT(window_bits_ == 0);
    ASSERT(!identity_mode_);
    if (bits == 0) return true;  // window disabled: legacy unbounded bias mode
    ASSERT(bits >= 20 && bits <= 47);
    const u64 size = u64(1) << bits;
    // One PROT_NONE reservation for the whole guest address space. PROT_NONE
    // anonymous pages commit nothing; every later guest mapping is carved out
    // of this range with MAP_FIXED, which is safe *because* the range is ours.
    void* base = mmap(nullptr,
                      size,
                      PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                      -1,
                      0);
    if (base == MAP_FAILED) {
        LOG_ERROR("GuestMemory: guest window reservation of {:#x} bytes failed, errno {}",
                  size,
                  errno);
        return false;
    }
    window_bits_ = bits;
    mask_ = size - 1;
    bias_ = reinterpret_cast<VAddr>(base);
    // Page-presence bitmap for the whole window: one bit per host page, so
    // 2^(bits-17) bytes -- 32 KiB at the default 32-bit window, 1 GiB at the
    // 47-bit maximum (calloc, so untouched pages cost nothing). This is what
    // makes the helper-side "is this guest range mapped?" probe lock-free;
    // see MappedBytesFrom.
    const u64 words = std::max<u64>(1, (size >> kHostPageShift) / 64);
    page_bitmap_.reset(static_cast<std::atomic<u64>*>(
            std::calloc(static_cast<size_t>(words), sizeof(std::atomic<u64>))));
    if (!page_bitmap_) {
        munmap(base, size);
        LOG_ERROR("GuestMemory: page-presence bitmap ({} words) allocation failed", words);
        window_bits_ = 0;
        mask_ = ~u64(0);
        bias_ = 0;
        return false;
    }
    // mmap arena: upper half of the window. The classic guest stack
    // (kGuestStackTop = 0x7FF00000) and the brk heap live below it.
    arena_base_ = size >> 1;
    LOG_INFO("GuestWindow: guest [0, {:#x}) -> host [{:#x}, {:#x}) mask {:#x}",
             size,
             bias_,
             bias_ + size,
             mask_);
    return true;
}

bool GuestMemory::MapFixed(VAddr addr, u64 size) {
    return MapFixedImpl(addr, size, false);
}

bool GuestMemory::MapFixedImpl(VAddr addr, u64 size, bool quiet_failure) {
    ASSERT(addr % kHostPageSize == 0);
    auto map_size = RoundHostPage(size);
    if (window_bits_ != 0) {
        // Windowed: the range must lie wholly inside the guest window. The
        // MAP_FIXED below can only ever land inside our own reservation.
        if (map_size == 0 || addr > mask_ || map_size > mask_ - addr + 1) {
            return false;
        }
        void* const host = ToHost(addr);
        auto* res = mmap(host,
                         map_size,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1,
                         0);
        if (res == MAP_FAILED) {
            LOG_ERROR("GuestMemory: window map failed at guest {:#x} size {:#x} errno {}",
                      addr,
                      map_size,
                      errno);
            return false;
        }
        TrackMap(addr, map_size);
        return true;
    }
    const VAddr host_addr = addr + bias_;
#if defined(__APPLE__)
    // mach_vm_allocate(VM_FLAGS_FIXED) fails with KERN_NO_SPACE when any
    // part of the range is already occupied — unlike MAP_FIXED it never
    // silently replaces existing host mappings. That matters because the
    // JIT code cache and other host allocations are placed freely by the
    // host VM, and a clobbering guest map could wipe them out.
    mach_vm_address_t target = host_addr;
    const kern_return_t kr = mach_vm_allocate(mach_task_self(), &target, map_size, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS || target != host_addr) {
        if (kr == KERN_SUCCESS) {
            mach_vm_deallocate(mach_task_self(), target, map_size);
        }
        LOG_ERROR("GuestMemory: fixed map failed at guest {:#x} (host {:#x}) size {:#x} kr {}",
                  addr,
                  host_addr,
                  map_size,
                  kr);
        return false;
    }
#else
    auto* res = mmap(reinterpret_cast<void*>(host_addr),
                     map_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                     -1,
                     0);
    if (res == MAP_FAILED) {
        if (!quiet_failure) {
            LOG_ERROR("GuestMemory: fixed map failed at guest {:#x} (host {:#x}) "
                      "size {:#x} errno {}",
                      addr,
                      host_addr,
                      map_size,
                      errno);
        }
        return false;
    }
    // Linux kernels before 4.17 may ignore the unknown NOREPLACE flag and
    // treat the address as a hint. Never accept such a result: identity mode
    // promises exact placement and must not silently become biased.
    if (reinterpret_cast<VAddr>(res) != host_addr) {
        munmap(res, map_size);
        errno = EOPNOTSUPP;
        if (!quiet_failure) {
            LOG_ERROR("GuestMemory: MAP_FIXED_NOREPLACE did not map the requested host "
                      "address {:#x} (got {}); Linux 4.17+ is required",
                      host_addr,
                      res);
        }
        return false;
    }
#endif
    TrackMap(addr, map_size);
    return true;
}

bool GuestMemory::FallBackIdentityToWindow() {
    ASSERT(identity_mode_);
    ASSERT(bias_ == 0);
    ASSERT(window_bits_ == 0);
    ASSERT(mapped_regions.empty());
    identity_mode_ = false;
    return ReserveWindow(kDefaultWindowBits);
}

VAddr GuestMemory::MapAnywhere(u64 size) {
    auto map_size = RoundHostPage(size);
    if (window_bits_ != 0) {
        VAddr addr;
        {
            std::shared_lock guard(mapped_regions_mutex);
            // First fit in the arena; fall back to the low half (below the
            // arena base) only if the arena is exhausted.
            addr = FindFreeLocked(arena_base_, map_size);
            if (!addr) addr = FindFreeLocked(kNullGuardEnd, map_size);
        }
        if (!addr) {
            LOG_ERROR("GuestMemory: guest window exhausted, cannot place {:#x} bytes", map_size);
            return 0;
        }
        return MapFixed(addr, map_size) ? addr : 0;
    }
    auto* res = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        LOG_ERROR("GuestMemory: map failed size {:#x} errno {}", map_size, errno);
        return 0;
    }
    // Report the guest address (host - bias).
    auto addr = reinterpret_cast<VAddr>(res) - bias_;
    TrackMap(addr, map_size);
    return addr;
}

bool GuestMemory::MapImageAnywhere(VAddr guest_start, u64 size) {
    ASSERT(guest_start % kHostPageSize == 0);
    auto map_size = RoundHostPage(size);
    if (window_bits_ != 0) {
        // Windowed: the guest keeps its linked addresses, which must fit in
        // the window. The bias is already installed by ReserveWindow.
        if (guest_start > mask_ || map_size > mask_ - guest_start + 1) {
            LOG_ERROR("GuestMemory: image [{:#x}, {:#x}) does not fit the {}-bit guest "
                      "window; raise it with SVM_GUEST_BITS (20..47)",
                      guest_start,
                      guest_start + map_size,
                      window_bits_);
            return false;
        }
        return MapFixed(guest_start, map_size);
    }
    if (identity_mode_) {
#if defined(__linux__)
        // The initial image span is one all-or-nothing mapping. If exact
        // placement collides, no guest mapping has been published yet, so it
        // is safe to select the same bounded window used by explicit bias mode
        // and retry through the common windowed branch.
        int identity_errno = 0;
        return TryIdentityWithFallback(
                [&] {
                    if (runtime::GetSvmConfig().mem_identity_test_collision) {
                        errno = EEXIST;
                        identity_errno = errno;
                        return false;
                    }
                    if (!MapFixedImpl(guest_start, map_size, true)) {
                        identity_errno = errno;
                        return false;
                    }
                    LOG_INFO("GuestMemory: identity image span guest=host [{:#x}, {:#x})",
                             guest_start,
                             guest_start + map_size);
                    return true;
                },
                [&] {
                    LOG_WARNING("GuestMemory: identity image span [{:#x}, {:#x}) is "
                                "unavailable (errno {}); falling back to the {}-bit "
                                "bounded bias window",
                                guest_start,
                                guest_start + map_size,
                                identity_errno,
                                kDefaultWindowBits);
                    if (!FallBackIdentityToWindow()) {
                        LOG_ERROR("GuestMemory: bounded bias fallback reservation failed");
                        return false;
                    }
                    return MapImageAnywhere(guest_start, map_size);
                });
#else
        PANIC("GuestMemory identity mode is only supported on Linux hosts");
#endif
    }
    ASSERT(bias_ == 0);  // the image reservation installs the bias; call once
    // Diagnostic fixed-stack repro: put the image at a stable high host hint,
    // which makes guest+bias for the classic 0x7ff00000 stack independent of
    // the executable/libSystem layout. mmap without MAP_FIXED remains
    // non-clobbering; require the requested hint so SVM_FORCE_FIXED_STACK=1
    // cannot silently fall back to the layout-sensitive path it is meant to
    // eliminate.
    constexpr VAddr kForcedImageHost = 0x20000000000;
    const bool force_fixed = runtime::GetSvmConfig().force_fixed_stack == "1";
    void* const hint =
            force_fixed ? reinterpret_cast<void*>(kForcedImageHost) : nullptr;
    auto* res = mmap(hint,
                     map_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1,
                     0);
    if (res == MAP_FAILED) {
        LOG_ERROR("GuestMemory: image reserve failed size {:#x} errno {}", map_size, errno);
        return false;
    }
    if (force_fixed && res != hint) {
        munmap(res, map_size);
        LOG_ERROR("GuestMemory: forced image host hint {:#x} unavailable (got {})",
                  kForcedImageHost,
                  res);
        return false;
    }
    SetBias(reinterpret_cast<VAddr>(res) - guest_start);
    TrackMap(guest_start, map_size);
    LOG_INFO("GuestMemory: image span guest {:#x} -> host {} (bias {:#x})",
             guest_start,
             res,
             bias_);
    return true;
}

void GuestMemory::Unmap(VAddr addr, u64 size) {
    auto base = RoundDownHostPage(addr);
    auto end = RoundHostPage(addr + size);
    if (end <= base) return;
    if (window_bits_ != 0) {
        if (base > mask_) return;
        if (end - 1 > mask_) end = mask_ + 1;
        // Keep the window reservation intact: overlay PROT_NONE anonymous
        // pages instead of punching a hole a host allocation could move into.
        // This also discards the old pages, so a later map is zero-filled.
        mmap(ToHost(base),
             end - base,
             PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
             -1,
             0);
        TrackUnmap(base, end - base);
        return;
    }

    // 恒等模式与翻译器共享宿主地址空间。Linux 允许对 guest 未映射区间执行
    // munmap；若把数值地址原样转交内核，可能误拆同址的翻译器映射，尤其是
    // JIT RX code cache。因此只释放本 GuestMemory 确实发布过的交集。
    std::unique_lock guard(mapped_regions_mutex);
    for (const auto& region : mapped_regions) {
        if (region.second <= base) continue;
        if (region.first >= end) break;
        const VAddr mapped_begin = std::max(base, region.first);
        const VAddr mapped_end = std::min(end, region.second);
        if (mapped_begin < mapped_end) {
            munmap(ToHost(mapped_begin), mapped_end - mapped_begin);
        }
    }
    TrackUnmapLocked(base, end - base);
}

bool GuestMemory::Protect(VAddr addr, u64 size, bool read, bool write, bool exec) {
    auto base = RoundDownHostPage(addr);
    auto end = RoundHostPage(addr + size);
    if (window_bits_ != 0) {
        if (base > mask_) return false;
        if (end - 1 > mask_) end = mask_ + 1;
        if (end <= base) return false;
    }
    int prot = PROT_NONE;
    if (read) prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (exec) prot |= PROT_EXEC;
    return mprotect(ToHost(base), end - base, prot) == 0;
}

bool GuestMemory::Read(void* dest, size_t addr, size_t size) {
    std::memcpy(dest, ToHostConst(addr), size);
    return true;
}

bool GuestMemory::Write(void* src, size_t addr, size_t size) {
    std::memcpy(ToHost(addr), src, size);
    return true;
}

std::string GuestMemory::ReadCString(VAddr addr, size_t max_len) {
    std::string result;
    result.reserve(64);
    for (size_t i = 0; i < max_len; ++i) {
        char c = Read<char>(addr + i);
        if (c == '\0') break;
        result.push_back(c);
    }
    return result;
}

// Sets/clears one bit per host page of [addr, addr+size). Callers hold
// mapped_regions_mutex, so the only concurrency is with lock-free readers;
// relaxed is enough because the pages themselves are already
// mapped/unmapped by the caller and a reader racing its own mmap has no
// defined answer to race with.
void GuestMemory::SetPageBits(VAddr addr, u64 size, bool present) {
    if (!page_bitmap_ || size == 0) return;
    const VAddr first = (addr & mask_) >> kHostPageShift;
    const VAddr last = ((addr & mask_) + size - 1) >> kHostPageShift;
    for (VAddr page = first; page <= last; ++page) {
        auto& word = page_bitmap_[page >> 6];
        const u64 bit = u64(1) << (page & 63);
        const u64 old = word.load(std::memory_order_relaxed);
        word.store(present ? (old | bit) : (old & ~bit), std::memory_order_relaxed);
    }
}

void GuestMemory::TrackMap(VAddr addr, u64 size) {
    if (size == 0) return;
    std::unique_lock guard(mapped_regions_mutex);
    const VAddr end = addr + size;
    // Replace any overlaps, then coalesce touching intervals. TrackUnmapLocked
    // clears the same page bits, so set them after it, not before.
    TrackUnmapLocked(addr, size);
    SetPageBits(addr, size, true);
    mapped_regions.emplace_back(addr, end);
    std::sort(mapped_regions.begin(), mapped_regions.end());
    std::vector<std::pair<VAddr, VAddr>> merged;
    merged.reserve(mapped_regions.size());
    for (const auto& r : mapped_regions) {
        if (!merged.empty() && r.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, r.second);
        } else {
            merged.push_back(r);
        }
    }
    mapped_regions = std::move(merged);
}

void GuestMemory::TrackUnmap(VAddr addr, u64 size) {
    if (size == 0) return;
    std::unique_lock guard(mapped_regions_mutex);
    TrackUnmapLocked(addr, size);
}

void GuestMemory::TrackUnmapLocked(VAddr addr, u64 size) {
    SetPageBits(addr, size, false);
    const VAddr end = addr + size;
    std::vector<std::pair<VAddr, VAddr>> out;
    out.reserve(mapped_regions.size());
    for (const auto& r : mapped_regions) {
        if (r.second <= addr || r.first >= end) {
            out.push_back(r);
            continue;
        }
        if (r.first < addr) out.emplace_back(r.first, addr);
        if (r.second > end) out.emplace_back(end, r.second);
    }
    mapped_regions = std::move(out);
}

bool GuestMemory::RangeIsMapped(VAddr addr, u64 size) const {
    if (page_bitmap_) {
        // Same answer as RangeIsMappedLocked, without the lock: mapped_regions
        // is kept coalesced, so "every page present" and "wholly inside one
        // region" are the same predicate.
        return size == 0 || MappedBytesFrom(addr, size) == size;
    }
    std::shared_lock guard(mapped_regions_mutex);
    return RangeIsMappedLocked(addr, size);
}

u64 GuestMemory::MappedBytesFromSlow(VAddr addr, u64 length) const {
    if (length == 0) return 0;
    if (window_bits_ != 0) {
        addr &= mask_;
        const u64 avail = mask_ - addr + 1;  // bytes to the window top
        if (length > avail) length = avail;
    } else if (addr + length < addr) {
        return 0;  // overflow: never a valid range
    }
    if (page_bitmap_) {
        u64 done = 0;
        VAddr cursor = addr;
        while (done < length) {
            if (!PageBit(cursor)) break;
            const u64 in_page = kHostPageSize - (cursor & (kHostPageSize - 1));
            const u64 chunk = in_page < length - done ? in_page : length - done;
            done += chunk;
            cursor += chunk;
        }
        return done;
    }
    std::shared_lock guard(mapped_regions_mutex);
    auto it = std::upper_bound(mapped_regions.begin(),
                               mapped_regions.end(),
                               addr,
                               [](VAddr a, const auto& p) { return a < p.first; });
    if (it == mapped_regions.begin()) return 0;
    --it;
    if (it->first > addr || it->second <= addr) return 0;
    const u64 room = it->second - addr;
    return room < length ? room : length;
}

VAddr GuestMemory::FindFreeLocked(VAddr from, u64 size) const {
    if (size == 0 || window_bits_ == 0) return 0;
    const VAddr limit = mask_ + 1;  // exclusive window end
    VAddr cursor = RoundHostPage(from);
    if (cursor > limit || size > limit - cursor) return 0;
    // mapped_regions is sorted and disjoint: push the candidate past every
    // region it overlaps until a large enough hole appears.
    for (const auto& r : mapped_regions) {
        if (r.second <= cursor) continue;      // entirely below the candidate
        if (r.first >= cursor + size) break;   // hole [cursor, r.first) fits
        cursor = RoundHostPage(r.second);
        if (cursor > limit || size > limit - cursor) return 0;
    }
    return cursor;
}

bool GuestMemory::RangeIsMappedLocked(VAddr addr, u64 size) const {
    if (size == 0) return true;
    if (addr + size < addr) return false;  // overflow
    if (window_bits_ != 0) {
        // Answer for the *effective* address: ToHost truncates to the window,
        // so validation has to truncate identically or the two disagree.
        // A range that would wrap over the window top is never valid.
        addr &= mask_;
        if (size > mask_ - addr + 1) return false;
    }
    // Find the last region with start <= addr.
    auto it = std::upper_bound(mapped_regions.begin(),
                               mapped_regions.end(),
                               addr,
                               [](VAddr a, const auto& p) { return a < p.first; });
    if (it == mapped_regions.begin()) return false;
    --it;
    return it->first <= addr && addr + size <= it->second;
}

bool GuestMemory::TryReadBytes(VAddr addr, std::span<u8> out) {
    std::shared_lock guard(mapped_regions_mutex);
    if (!RangeIsMappedLocked(addr, out.size())) return false;
    std::memcpy(out.data(), ToHostConst(addr), out.size());
    return true;
}

bool GuestMemory::TryWriteBytes(VAddr addr, std::span<const u8> data) {
    std::shared_lock guard(mapped_regions_mutex);
    if (!RangeIsMappedLocked(addr, data.size())) return false;
    std::memcpy(ToHost(addr), data.data(), data.size());
    return true;
}

bool GuestMemory::TryReadCString(VAddr addr, std::string& out, size_t max_len) {
    std::shared_lock guard(mapped_regions_mutex);
    out.clear();
    for (size_t i = 0; i < max_len; ++i) {
        if (!RangeIsMappedLocked(addr + i, 1)) return false;
        char c;
        std::memcpy(&c, ToHostConst(addr + i), 1);
        if (c == '\0') return true;
        out.push_back(c);
    }
    return false;  // no terminator within max_len
}

}  // namespace swift::linux
