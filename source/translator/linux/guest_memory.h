//
// Guest (ARM64 / x86_64 Linux) memory management for the SwiftVM linux loader.
//
// Address model: the guest runs at its *linked* virtual addresses, but the
// host cannot necessarily map there (macOS pagezero blocks the low 4GB), so
// every guest address G is backed by host memory at G + bias ("memory_base"
// mode — the runtime backend applies the same bias in JIT/interp memory
// accesses via Config::memory_base / State::pt). With bias == 0 this
// degenerates to the old identity mapping.
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

#include <cstring>
#include <span>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>
#include "base/common_funcs.h"
#include "base/types.h"
#include "runtime/include/config.h"

namespace swift::linux {

class GuestMemory : public runtime::MemoryInterface {
public:
    // Guest page size reported to the guest (AT_PAGESZ, mmap/brk rounding).
    static constexpr u64 kGuestPageSize = 0x1000;
    // Host page granularity on macOS arm64 is 16KB; all mappings must be
    // aligned to this.
    static constexpr u64 kHostPageSize = 0x4000;

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
    // Tracked guest mappings (sorted, disjoint [start, end) host-page
    // granularity intervals, in *guest* addresses). Maintained by
    // MapFixed/MapAnywhere/Unmap.
    void TrackMap(VAddr addr, u64 size);
    void TrackUnmap(VAddr addr, u64 size);
    void TrackUnmapLocked(VAddr addr, u64 size);
    [[nodiscard]] bool RangeIsMappedLocked(VAddr addr, u64 size) const;
    // First free [addr, addr+size) hole at or above `from` inside the window.
    [[nodiscard]] VAddr FindFreeLocked(VAddr from, u64 size) const;
    mutable std::shared_mutex mapped_regions_mutex;
    std::vector<std::pair<VAddr, VAddr>> mapped_regions;
    u64 bias_{};
    // Bounded guest window. window_bits_ == 0 => disabled, mask_ == ~0.
    u32 window_bits_{};
    u64 mask_{~u64(0)};
    VAddr arena_base_{};
};

}  // namespace swift::linux
