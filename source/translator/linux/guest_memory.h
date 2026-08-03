//
// Guest (ARM64 / x86_64 Linux) memory management for the SwiftVM linux loader.
//
// Address model:
//  - Linux defaults to identity mode: mappings are placed directly at G
//    without replacement. This removes the runtime bias, but a guest wild
//    pointer may then name an unrelated host mapping.
//  - macOS always uses the bounded bias window. Linux selects the same mode
//    explicitly with SVM_MEM_IDENTITY=0 or SVM_GUEST_BITS.
//
// ALL public methods of this class take and return *guest* addresses; the
// bias conversion is centralized here (ToHost/ToGuest) so callers (loader,
// stack setup, syscall emulation) never see host pointers.
//
// Note: guest page protections are intentionally *not* honored — the host
// never executes guest code (the JIT reads it as data), so all guest pages
// stay host RW. Guest PROT_EXEC is a no-op for us.
//

#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <span>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>
#include "base/common_funcs.h"
#include "base/types.h"
#include "runtime/include/config.h"

namespace swift::linux {

struct GuestMemoryLaunchPolicy {
    bool identity{};
};

// Keep environment precedence in a pure helper so the four launch modes can
// be tested without reserving a process address space. On macOS linux_host is
// false, so the identity selector is ignored; the launcher still parses its
// existing SVM_GUEST_BITS window-size override after this decision.
inline GuestMemoryLaunchPolicy SelectGuestMemoryLaunchPolicy(
        bool linux_host,
        const char* identity_value,
        const char* guest_bits_value) {
    if (!linux_host || guest_bits_value) {
        return {.identity = false};
    }
    const bool explicitly_disabled = identity_value &&
            (std::strcmp(identity_value, "0") == 0 ||
             std::strcmp(identity_value, "OFF") == 0 ||
             std::strcmp(identity_value, "off") == 0);
    return {.identity = !explicitly_disabled};
}

class GuestMemory : public runtime::MemoryInterface {
public:
    // Guest page size reported to the guest (AT_PAGESZ, mmap/brk rounding).
    static constexpr u64 kGuestPageSize = 0x1000;
    // macOS arm64 使用 16KB 宿主页；当前 Linux 启动器运行在 4KB 页环境。
    // 不能把 Darwin 粒度带到 identity：mmap 只保证按 Linux 宿主页对齐，
    // 用 16KB 向下取整会把相邻的翻译器映射一起交给 munmap。
#if defined(__APPLE__)
    static constexpr u64 kHostPageSize = 0x4000;
    static constexpr u32 kHostPageShift = 14;
#else
    static constexpr u64 kHostPageSize = 0x1000;
    static constexpr u32 kHostPageShift = 12;
#endif

    // --- Bounded guest window (address-space isolation) -------------------
    // Default number of guest address bits. 32 is special-cased everywhere it
    // matters: the arm64 JIT can express `host = pt + zext32(guest)` with the
    // register-offset-with-extend addressing mode, so truncation is free.
    static constexpr u32 kDefaultWindowBits = 32;
    // The mmap arena starts at half the window and grows up; the classic guest
    // stack (0x7FF00000) and brk live below it.
    static constexpr u64 kNullGuardEnd = 0x10000;

    GuestMemory() = default;
    ~GuestMemory() = default;

    // Guest->host bias (host address = (guest address & mask) + bias). Must
    // be host-page aligned. Set once by the loader after reserving the image
    // span; 0 means identity mapping.
    void SetBias(u64 bias) { bias_ = bias; }
    [[nodiscard]] u64 GetBias() const { return bias_; }

    // Select Linux host identity mapping before any reservation or mapping.
    // The launcher never calls this on macOS.
    void EnableIdentityMode() {
        ASSERT(bias_ == 0);
        ASSERT(window_bits_ == 0);
        identity_mode_ = true;
    }
    [[nodiscard]] bool IdentityMode() const { return identity_mode_; }

    // Shared control-flow primitive for the initial Linux ET_EXEC mapping.
    // The fallback callback is invoked exactly once only after the identity
    // attempt fails; keeping this generic allows collision injection tests to
    // cover the transaction without platform-specific mmap side effects.
    template <typename IdentityAttempt, typename BiasFallback>
    static bool TryIdentityWithFallback(IdentityAttempt&& identity_attempt,
                                        BiasFallback&& bias_fallback) {
        if (identity_attempt()) {
            return true;
        }
        return bias_fallback();
    }

    // Reserves the whole guest window ([0, 2^bits) guest) as one PROT_NONE
    // host region and installs the bias. Every later mapping is carved out of
    // this reservation with MAP_FIXED, so no guest address — masked to `bits`
    // bits — can ever name host memory outside it. bits == 0 disables the
    // window and keeps the legacy unbounded bias mode.
    bool ReserveWindow(u32 bits);
    [[nodiscard]] bool Windowed() const { return window_bits_ != 0; }
    [[nodiscard]] u32 WindowBits() const { return window_bits_; }
    // Address mask applied to every guest address before the bias is added.
    // ~0 when the window is disabled (legacy behaviour).
    [[nodiscard]] u64 Mask() const { return mask_; }
    [[nodiscard]] u64 WindowSize() const { return window_bits_ ? (u64(1) << window_bits_) : 0; }

    // Guest address -> host pointer (the ONLY place the bias is applied).
    [[nodiscard]] void* ToHost(VAddr guest_addr) const {
        return reinterpret_cast<void*>((guest_addr & mask_) + bias_);
    }
    [[nodiscard]] const void* ToHostConst(VAddr guest_addr) const {
        return reinterpret_cast<const void*>((guest_addr & mask_) + bias_);
    }
    // Host pointer -> guest address.
    [[nodiscard]] VAddr ToGuest(const void* host_ptr) const {
        return reinterpret_cast<VAddr>(host_ptr) - bias_;
    }

    // Map anonymous zero pages at an exact guest address (host map at
    // guest + bias). Returns false on failure. In windowed mode the range
    // must lie inside the window; the map is carved out of the reservation
    // and therefore replaces whatever guest pages were there (callers that
    // need Linux' "fail if occupied" semantics test RangeIsMapped first).
    bool MapFixed(VAddr addr, u64 size);

    // Map anonymous pages at a free guest address; returns the *guest*
    // address or 0 on failure. Windowed: first fit in the mmap arena.
    VAddr MapAnywhere(u64 size);

    // Reserves the guest image span at a host-chosen location and installs
    // the resulting guest->host bias. Must be called before any other
    // mapping (the bias is address-space wide). guest_start must be
    // host-page aligned. Returns false on failure.
    bool MapImageAnywhere(VAddr guest_start, u64 size);

    void Unmap(VAddr addr, u64 size);

    // Best-effort protection change; rounds to host page granularity.
    bool Protect(VAddr addr, u64 size, bool read, bool write, bool exec);

    // runtime::MemoryInterface — bias translation (instruction fetch).
    bool Read(void* dest, size_t addr, size_t size) override;
    bool Write(void* src, size_t addr, size_t size) override;
    // Instruction fetch, and the ONLY guest access the frontend makes from
    // host code. It must be validated: the bias is a plain add, so an
    // unmapped guest address yields a host pointer into whatever the host
    // happens to have there — a wild guest branch target would fault the HOST
    // translator (host pc outside every JIT buffer, so runtime.cpp's
    // HandleFault cannot recover it) instead of killing only the guest.
    // Returning nullptr routes the caller to ExitReason::PageFatal; every
    // caller in runtime/frontend/x86 already handles it.
    void* GetPointer(void* src) override {
        const auto guest = reinterpret_cast<VAddr>(src);
        if (!RangeIsMapped(guest, 1)) {
            return nullptr;
        }
        return ToHost(guest);
    }

    // Typed helpers for guest memory access.
    template <typename T> T Read(VAddr addr) {
        T t;
        Read(&t, addr, sizeof(T));
        return t;
    }

    template <typename T> void Write(VAddr addr, const T& value) {
        std::memcpy(ToHost(addr), &value, sizeof(T));
    }

    void WriteBytes(VAddr addr, std::span<const u8> data) {
        std::memcpy(ToHost(addr), data.data(), data.size());
    }

    void ReadBytes(VAddr addr, std::span<u8> out) {
        std::memcpy(out.data(), ToHostConst(addr), out.size());
    }

    // Copy a NUL-terminated string out of guest memory (bounded).
    std::string ReadCString(VAddr addr, size_t max_len = 4096);

    // --- Safe (validated) accessors for syscall emulation -----------------
    // These consult the tracked mapping set first, so a wild guest pointer
    // yields false (=> -EFAULT) instead of crashing the host process.

    // True if every byte of [addr, addr+size) lies inside one mapped region.
    bool RangeIsMapped(VAddr addr, u64 size) const;

    // Number of contiguous mapped bytes starting at `addr`, capped at
    // `length` (and at the window end). 0 means `addr` itself is unmapped.
    //
    // This is the probe the *helpers* use — x87/fxsave and the rep-string
    // walks, which dereference guest memory from host frames a fault cannot
    // be recovered from — so it must be cheap enough to sit in front of a
    // `rep movsb` of four bytes. With the window enabled it is a lock-free
    // read of a page-presence bitmap (one bit per 16 KiB host page); the
    // shared_mutex + interval binary search is only the unwindowed fallback.
    //
    // The overwhelmingly common shape — a windowed access that fits in one
    // host page — is inlined here; everything else goes out of line.
    [[nodiscard]] u64 MappedBytesFrom(VAddr addr, u64 length) const {
        if (page_bitmap_) {
            const VAddr base = addr & mask_;
            const u64 in_page = kHostPageSize - (base & (kHostPageSize - 1));
            if (length <= in_page) {
                return PageBit(base) ? length : 0;
            }
        }
        return MappedBytesFromSlow(addr, length);
    }
    [[nodiscard]] u64 MappedBytesFromSlow(VAddr addr, u64 length) const;

    bool TryReadBytes(VAddr addr, std::span<u8> out);
    bool TryWriteBytes(VAddr addr, std::span<const u8> data);

    template <typename T> bool TryRead(VAddr addr, T& out) {
        return TryReadBytes(addr, {reinterpret_cast<u8*>(&out), sizeof(T)});
    }

    template <typename T> bool TryWrite(VAddr addr, const T& value) {
        return TryWriteBytes(addr, {reinterpret_cast<const u8*>(&value), sizeof(T)});
    }

    // Reads a NUL-terminated string; false if any byte (incl. the
    // terminator) is unmapped or max_len is exceeded.
    bool TryReadCString(VAddr addr, std::string& out, size_t max_len = 4096);

    static constexpr u64 RoundGuestPage(u64 v) { return (v + kGuestPageSize - 1) & ~(kGuestPageSize - 1); }
    static constexpr u64 RoundHostPage(u64 v) { return (v + kHostPageSize - 1) & ~(kHostPageSize - 1); }
    static constexpr u64 RoundDownHostPage(u64 v) { return v & ~(kHostPageSize - 1); }

private:
    bool MapFixedImpl(VAddr addr, u64 size, bool quiet_failure);
    bool FallBackIdentityToWindow();
    // Tracked guest mappings (sorted, disjoint [start, end) host-page
    // granularity intervals, in *guest* addresses). Maintained by
    // MapFixed/MapAnywhere/Unmap.
    void TrackMap(VAddr addr, u64 size);
    void TrackUnmap(VAddr addr, u64 size);
    void TrackUnmapLocked(VAddr addr, u64 size);
    [[nodiscard]] bool RangeIsMappedLocked(VAddr addr, u64 size) const;
    // First free [addr, addr+size) hole at or above `from` inside the window.
    [[nodiscard]] VAddr FindFreeLocked(VAddr from, u64 size) const;
    // Page-presence bitmap, one bit per host page of the window; nullptr when
    // the window is disabled. Written only under mapped_regions_mutex (so the
    // bitmap and mapped_regions never disagree for a caller that is not
    // racing its own mmap), read without any lock.
    void SetPageBits(VAddr addr, u64 size, bool present);
    [[nodiscard]] bool PageBit(VAddr addr) const {
        const u64 page = (addr & mask_) >> kHostPageShift;
        return (page_bitmap_[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1;
    }
    static_assert(kHostPageSize == (u64(1) << kHostPageShift));
    // calloc, not new[]: at the 47-bit maximum the bitmap is 1 GiB and
    // value-initializing it would fault in every page. calloc hands back
    // fresh zero pages, and all-zero bytes are a valid representation of
    // std::atomic<u64>{0}.
    struct BitmapFree {
        void operator()(std::atomic<u64>* p) const noexcept { std::free(p); }
    };
    std::unique_ptr<std::atomic<u64>[], BitmapFree> page_bitmap_;
    mutable std::shared_mutex mapped_regions_mutex;
    std::vector<std::pair<VAddr, VAddr>> mapped_regions;
    u64 bias_{};
    // Linux host identity mode. This is distinct from unwindowed bias:
    // MapImageAnywhere maps at guest_start rather than choosing a host base.
    bool identity_mode_{};
    // Bounded guest window. window_bits_ == 0 => disabled, mask_ == ~0.
    u32 window_bits_{};
    u64 mask_{~u64(0)};
    VAddr arena_base_{};
};

}  // namespace swift::linux
