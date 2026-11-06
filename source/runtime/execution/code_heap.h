#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "mem_map.h"
#include "runtime/backend/code.h"

namespace swift::runtime::execution {
struct HostBackendOps;
using NativeCode = backend::NativeCode;

// The only owner of native slabs and actual recovery buffers. Entries in the
// PC directory borrow these ranges; an entry never changes the unit base.
class CodeHeap {
public:
    using ReadyResolver = std::uintptr_t(*)(void*, std::uint64_t) noexcept;
    CodeHeap(const HostBackendOps&, std::size_t page_size) noexcept;
    ~CodeHeap();
    CodeHeap(const CodeHeap&) = delete;
    CodeHeap& operator=(const CodeHeap&) = delete;

    NativeCode Publish(std::span<const std::uint32_t>, std::vector<backend::FaultSite>,
                       std::span<const backend::LinkSite>, ReadyResolver = nullptr, void* = nullptr);
    // Caller has revoked all entries/links and ended native/fault borrows.
    // Failed mappings remain owned here; no global retained-owner registry.
    MappingRelease Release() noexcept;
    std::size_t Slabs() const noexcept;
    std::size_t DualSlabs() const noexcept;
    std::size_t ReservedBytes() const noexcept;

private:
    struct Slab { MemMap mapping; std::size_t used{}; };
    const HostBackendOps& backend;
    std::size_t page_size;
    std::vector<Slab> slabs;
    std::vector<std::vector<backend::FaultSite>> fault_tables;
};
} // namespace swift::runtime::execution
