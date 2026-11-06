#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dispatch.h"
#include "host_backend.h"

namespace swift::runtime::execution {

// One domain's authoritative PC membership. Units borrow CodeHeap storage;
// aliases share a code_id and always retain the original range/fault base.
class CodeCache {
public:
    CodeCache(NativeCode::Function pending, const HostBackendOps& backend) noexcept;
    NativeCode::Function Find(std::uint64_t pc, NativeCode* unit = nullptr) const noexcept;
    const NativeCode* FindHost(std::uintptr_t pc) const noexcept;
    void InsertReady(std::uint64_t pc, NativeCode, std::span<const backend::EntryPoint> extra);
    void AddStaticLink(const NativeCode&, const backend::LinkSite&);
    void AddIndirectSite(const NativeCode&, const backend::LinkSite&);
    void ResetLinks() noexcept;
    void Clear() noexcept;
    void Release() noexcept;

    std::size_t Entries() const noexcept { return entries; }
    std::size_t Occupied() const noexcept { return occupied; }
    std::size_t Slots() const noexcept { return slots.size(); }
    DispatchSlot* Data() noexcept { return slots.empty() ? &empty : slots.data(); }
    DispatchSlot* EmptyData() noexcept { return &empty; }
    std::uint64_t Mask() const noexcept { return slots.empty() ? 0 : slots.size() - 1; }
    std::size_t Pending() const noexcept { return pending_count; }
    std::size_t PendingTargets() const noexcept { return pending_targets; }
    std::size_t LinkRows() const noexcept { return links.size(); }
    std::size_t LinkCapacity() const noexcept { return links.capacity(); }
    std::uint64_t Patched() const noexcept { return patched; }
    std::uint64_t Allocations() const noexcept { return allocations; }

private:
    struct Relocation {
        std::uintptr_t* writable;
        const std::uintptr_t* executable;
        std::uint32_t next{UINT32_MAX}, branch_back_bytes{};
        backend::LinkKind kind{backend::LinkKind::Static};
    };
    static_assert(sizeof(Relocation) == 32);
    const HostBackendOps& backend;
    DispatchSlot empty;
    std::vector<DispatchSlot> slots;
    std::vector<NativeCode> codes;
    // The only actual relocation directory, including already-ready links.
    std::vector<Relocation> links;
    std::size_t entries{}, occupied{}, pending_count{}, pending_targets{};
    std::uint64_t allocations{}, patched{};
    void BindReady(std::uint64_t, std::uint32_t id, NativeCode::Function);
    DispatchSlot& Reserve(std::uint64_t);
    void Grow();
    void Patch(const Relocation&, NativeCode::Function) noexcept;
    static std::size_t Index(std::uint64_t, std::size_t) noexcept;
    static std::size_t EmptyOrMatching(const std::vector<DispatchSlot>&, std::uint64_t) noexcept;
};
} // namespace swift::runtime::execution
