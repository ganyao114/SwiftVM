#include "home_index.h"

#include "allocation.h"

namespace swift::runtime::compile {
AllocationView GPRHomeIndex::Locations() const { return result.View(); }

void GPRHomeIndex::Prepare() {
    if (!dirty) return;
    intervals.Clear();
    for (u32 id = 0; id < result.gpr_placements.size(); ++id)
        intervals.Add(id, result.gpr_placements[id].fixed);
    intervals.Finish();
    dirty = false;
}

bool GPRHomeIndex::Conflicts(u16 home, u32 begin, u32 end, u32 ignored, u32 ignored_second) const {
    ASSERT(!dirty);
    return intervals.Conflicts(home, begin, end, ignored, ignored_second);
}
}  // namespace swift::runtime::compile
