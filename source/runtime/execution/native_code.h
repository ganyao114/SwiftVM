#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "runtime/backend/arm64/compile.h"

namespace swift::runtime::execution {

struct NativeCode {
    using Function = std::uint64_t(*)(std::uint8_t*);
    Function entry{};
    std::size_t bytes{};
    std::uint8_t* writable{}; // Null for the existing single-mapping W^X fallback.
    std::span<const backend::arm64::FaultSite> faults;
};
static_assert(sizeof(NativeCode) == 40 && std::is_trivially_destructible_v<NativeCode>);

// Static LinkSite: ordinary LDR/B26 head, or an imm19 conditional head with
// its own LDR/BR gate. DATA is an aligned u64 target; near heads link directly.
// Invalidation restores the original load and conditional-to-gate transfer
// and its own cold stub before the owning arena is retired. The stub starts
// immediately after the DATA; both aliases name the same actual relocation.
// back_words is the explicit DATA-to-head distance from that LinkSite. An
// ordinary head is LDR/B26 (2 or 3 words); an imm19 head uses an even cold
// LDR/BR gate immediately before DATA. Patch/Unlink preserve its cond/Rt bits.
void PatchNativeLink(std::uintptr_t* writable, const std::uintptr_t* executable,
                     std::uintptr_t target, std::uint32_t back_words) noexcept;
void UnlinkNativeLink(std::uintptr_t* writable, const std::uintptr_t* executable,
                      std::uint32_t back_words) noexcept;

// Single guest thread: publishing and invalidation happen after native code
// returns to the dispatcher. Cache items borrow entries; slabs own mappings.
class CodeArena {
public:
    CodeArena() = default;
    ~CodeArena();
    CodeArena(const CodeArena&) = delete;
    CodeArena& operator=(const CodeArena&) = delete;
    NativeCode Publish(std::span<const std::uint32_t>, std::vector<backend::arm64::FaultSite> faults = {},
                       std::span<const backend::arm64::LinkSite> links = {});
    void Clear() noexcept;
    std::size_t Slabs() const noexcept { return slabs.size(); }
    std::size_t DualSlabs() const noexcept;
    std::size_t ReservedBytes() const noexcept;

private:
    struct Slab { std::uint8_t* write; std::uint8_t* execute; std::size_t size, used; bool dual; };
    std::vector<Slab> slabs;
    // Inner vector buffers stay put when this directory moves. NativeCode
    // borrows only those buffers, never an outer-vector element.
    std::vector<std::vector<backend::arm64::FaultSite>> fault_tables;
};

} // namespace swift::runtime::execution
