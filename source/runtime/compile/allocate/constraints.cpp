#include "constraints.h"

#include <algorithm>

namespace swift::runtime::compile {

void FixedGPRHomeIndex::Clear() { intervals.clear(); }

void FixedGPRHomeIndex::Add(u32 value, const FixedGPRDefinition& definition) {
    if (definition.home == UINT16_MAX) return;
    ASSERT(definition.home < 32);
    if (definition.end > value)
        intervals.push_back({value, value, definition.end, 0, definition.home});
    if (definition.transferred_home != UINT16_MAX && definition.last_use > definition.transfer) {
        ASSERT(definition.transferred_home < 32);
        intervals.push_back(
                {value, definition.transfer, definition.last_use, 0, definition.transferred_home});
    }
}

void FixedGPRHomeIndex::Finish() {
    std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
        return a.home < b.home || (a.home == b.home && a.begin < b.begin);
    });
    u16 home = UINT16_MAX;
    u32 longest = 0;
    for (auto& interval : intervals) {
        if (interval.home != home) {
            home = interval.home;
            longest = 0;
        }
        longest = std::max(longest, interval.end);
        interval.prefix_end = longest;
    }
}

bool FixedGPRHomeIndex::Conflicts(
        u16 home, u32 begin, u32 end, u32 ignored, u32 ignored_second) const {
    ASSERT(home < 32 && begin <= end);
    const auto first = std::lower_bound(
            intervals.begin(), intervals.end(), home, [](const auto& item, u16 key) {
                return item.home < key;
            });
    const auto last = std::upper_bound(first, intervals.end(), home, [](u16 key, const auto& item) {
        return key < item.home;
    });
    auto next = std::upper_bound(first, last, begin, [](u32 point, const auto& item) {
        return point < item.prefix_end;
    });
    for (; next != last && next->begin <= end; ++next)
        if (next->owner != ignored && next->owner != ignored_second && next->end > begin)
            return true;
    return false;
}

}  // namespace swift::runtime::compile
