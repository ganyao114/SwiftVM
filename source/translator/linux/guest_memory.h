//
// Guest (ARM64 Linux) memory management for the SwiftVM linux loader.
//
// The runtime backend uses guest virtual addresses directly as host pointers
// (identity mapping, Config::page_table / memory_base are null), so "guest
// memory" here is simply memory mapped into the host process at the guest
// address. This class wraps the host mmap/munmap/mprotect needed to place
// the guest image, stack and dynamic allocations, and implements
// runtime::MemoryInterface for explicit guest memory accesses (syscall
// buffer copies, stack setup, ...).
//
// Note: guest page protections are intentionally *not* honored — the host
// never executes guest code (the JIT reads it as data), so all guest pages
// stay host RW. Guest PROT_EXEC is a no-op for us.
//

#pragma once

#include <cstring>
#include <span>
#include <string>
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

    GuestMemory() = default;
    ~GuestMemory() = default;

    // Map anonymous zero pages at an exact guest address (host MAP_FIXED).
    // Returns false on failure.
    bool MapFixed(VAddr addr, u64 size);

    // Map anonymous pages at a host-chosen address; returns the address
    // (which is also the guest address, by identity mapping) or 0 on failure.
    VAddr MapAnywhere(u64 size);

    void Unmap(VAddr addr, u64 size);

    // Best-effort protection change; rounds to host page granularity.
    bool Protect(VAddr addr, u64 size, bool read, bool write, bool exec);

    // runtime::MemoryInterface — identity translation.
    bool Read(void* dest, size_t addr, size_t size) override;
    bool Write(void* src, size_t addr, size_t size) override;
    void* GetPointer(void* src) override { return src; }

    // Typed helpers for guest memory access.
    template <typename T> T Read(VAddr addr) {
        T t;
        Read(&t, addr, sizeof(T));
        return t;
    }

    template <typename T> void Write(VAddr addr, const T& value) {
        std::memcpy(reinterpret_cast<void*>(addr), &value, sizeof(T));
    }

    void WriteBytes(VAddr addr, std::span<const u8> data) {
        std::memcpy(reinterpret_cast<void*>(addr), data.data(), data.size());
    }

    void ReadBytes(VAddr addr, std::span<u8> out) {
        std::memcpy(out.data(), reinterpret_cast<const void*>(addr), out.size());
    }

    // Copy a NUL-terminated string out of guest memory (bounded).
    std::string ReadCString(VAddr addr, size_t max_len = 4096);

    static constexpr u64 RoundGuestPage(u64 v) { return (v + kGuestPageSize - 1) & ~(kGuestPageSize - 1); }
    static constexpr u64 RoundHostPage(u64 v) { return (v + kHostPageSize - 1) & ~(kHostPageSize - 1); }
    static constexpr u64 RoundDownHostPage(u64 v) { return v & ~(kHostPageSize - 1); }
};

}  // namespace swift::linux
