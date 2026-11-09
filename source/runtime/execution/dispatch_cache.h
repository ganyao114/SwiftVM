#pragma once

#include <cstddef>
#include <cstdint>

#include "mem_map.h"
#include "runtime/backend/code.h"
#include "runtime/backend/dispatch_layout.inc"

namespace swift::runtime::execution {

struct DispatchEntry {
    std::uint64_t pc{};
    backend::NativeCode::Function entry{};
};

static_assert(sizeof(DispatchEntry) == (1u << SVM_DISPATCH_L1_ENTRY_SHIFT));
static_assert(offsetof(DispatchEntry, pc) == 0 && offsetof(DispatchEntry, entry) == 8);

// A guest thread's disposable ready cache. The VM owns the code and shared L2.
// The mapping starts as zero pages; only slot zero needs a different empty tag.
class DispatchCache {
public:
    static constexpr std::size_t Entries = std::size_t{1} << SVM_DISPATCH_L1_BITS;
    static constexpr std::size_t Bytes = Entries * sizeof(DispatchEntry);

    DispatchCache() = default;
    ~DispatchCache();
    DispatchCache(const DispatchCache&) = delete;
    DispatchCache& operator=(const DispatchCache&) = delete;

    static constexpr std::size_t Index(std::uint64_t pc) noexcept { return pc & (Entries - 1); }

    void Ensure();
    void Reset();
    // The caller supplies a published ready entry, never a pending thunk.
    void Fill(std::uint64_t pc, backend::NativeCode::Function entry) noexcept;
    void ClearKey(std::uint64_t pc) noexcept;
    MappingRelease Release() noexcept;

    DispatchEntry* Data() const noexcept { return data; }

private:
    DispatchEntry* data{};
};

// All threads in one VM may select this same protected range independently.
// It reserves the whole L1 index span and never contains cache entries.
class DispatchInterrupt {
public:
    DispatchInterrupt() = default;
    ~DispatchInterrupt();
    DispatchInterrupt(const DispatchInterrupt&) = delete;
    DispatchInterrupt& operator=(const DispatchInterrupt&) = delete;
    void Ensure();
    MappingRelease Release() noexcept;

    DispatchEntry* Data() const noexcept { return data; }

private:
    DispatchEntry* data{};
};

}  // namespace swift::runtime::execution
