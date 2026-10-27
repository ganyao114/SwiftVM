#pragma once

#include "constraints.h"

namespace swift::runtime::compile {
class Allocation;
class AllocationView;
class AllocationWriter;

// A preparation-owned index over this allocation's fixed declarations.
// Only the writer prepares/invalidates it; borrowed queries are read-only.
class GPRHomeIndex final {
public:
    GPRHomeIndex(const GPRHomeIndex&) = delete;
    GPRHomeIndex& operator=(const GPRHomeIndex&) = delete;
    [[nodiscard]] bool Ready() const { return !dirty; }
    [[nodiscard]] AllocationView Locations() const;
    [[nodiscard]] bool Conflicts(u16 home,
                                 u32 begin,
                                 u32 end,
                                 u32 ignored = UINT32_MAX,
                                 u32 ignored_second = UINT32_MAX) const;

private:
    friend class AllocationWriter;
    explicit GPRHomeIndex(const Allocation& result) : result(result) {}
    void Prepare();
    void Invalidate() { dirty = true; }
    const Allocation& result;
    FixedGPRHomeIndex intervals;
    bool dirty{true};
};
}  // namespace swift::runtime::compile
