#include "preservation_index.h"

#include <algorithm>
#include <array>
#include <utility>
#include "allocation.h"

namespace swift::runtime::compile {
AllocationView GPRPreservationIndex::Locations() const { return result.View(); }

void GPRPreservationIndex::Prepare() {
    if (!dirty) return;
    ranges.clear();
    std::array<u32, 32> last;
    last.fill(UINT32_MAX);
    for (u32 id = 0; id < result.gpr_placements.size(); ++id) {
        const auto& fixed = result.gpr_placements[id].fixed;
        if (fixed.preserved && fixed.transfer > id) {
            ASSERT(fixed.home < 32);
            const auto previous = last[fixed.home];
            // IDs are visited in order, so each home's new prefix can only
            // overlap its last window. Store disjoint windows immediately.
            if (previous != UINT32_MAX && id <= ranges[previous].end) {
                ranges[previous].end = std::max(ranges[previous].end, fixed.transfer);
                continue;
            }
            last[fixed.home] = static_cast<u32>(ranges.size());
            ranges.push_back({id, fixed.transfer, fixed.home});
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.home < right.home || (left.home == right.home && left.begin < right.begin);
    });
    dirty = false;
}

bool GPRPreservationIndex::Conflicts(u16 home, u32 begin, u32 end) const {
    ASSERT(!dirty && home < 32 && begin <= end);
    // Merging makes end strictly increasing within each home. Find the first
    // range whose end is after begin, then test the inclusive consumer end.
    const auto next =
            std::upper_bound(ranges.begin(),
                             ranges.end(),
                             std::pair{home, begin},
                             [](const auto& point, const auto& range) {
                                 return point.first < range.home ||
                                        (point.first == range.home && point.second < range.end);
                             });
    return next != ranges.end() && next->home == home && next->begin <= end;
}

}  // namespace swift::runtime::compile
