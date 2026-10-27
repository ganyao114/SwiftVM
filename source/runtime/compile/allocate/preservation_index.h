#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::compile {
class Allocation;
class AllocationView;
class AllocationWriter;

// One preparation-owned physical index. Readers borrow this interface without
// gaining location-editing capabilities. The writer invalidates it at exactly
// the same placement changes as before; frozen results do not retain it.
class GPRPreservationIndex final {
public:
    GPRPreservationIndex(const GPRPreservationIndex&) = delete;
    GPRPreservationIndex& operator=(const GPRPreservationIndex&) = delete;
    GPRPreservationIndex(GPRPreservationIndex&&) = delete;
    GPRPreservationIndex& operator=(GPRPreservationIndex&&) = delete;
    [[nodiscard]] AllocationView Locations() const;
    [[nodiscard]] bool Ready() const { return !dirty; }
    [[nodiscard]] bool Conflicts(u16 home, u32 begin, u32 end) const;

private:
    friend class AllocationWriter;
    explicit GPRPreservationIndex(const Allocation& result) : result(result) {}
    void Prepare();
    void Invalidate() { dirty = true; }
    const Allocation& result;
    struct Range {
        u32 begin, end;
        u16 home;
    };
    Vector<Range> ranges;
    bool dirty{true};
};
}  // namespace swift::runtime::compile
