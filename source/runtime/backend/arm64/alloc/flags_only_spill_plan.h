#pragma once
#include <algorithm>
#include <functional>
#include <vector>
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::arm64 {
struct FlagsOnlySpillUse {
    const ir::Inst* consumer{};
    u32 producer_id{};
};
// Built during final target verification; thereafter consumed through const actions.
struct FlagsOnlySpillPlan {
    std::vector<FlagsOnlySpillUse> uses;
    static bool Less(const FlagsOnlySpillUse& a, const FlagsOnlySpillUse& b) {
        if (a.consumer != b.consumer) return std::less<const ir::Inst*>{}(a.consumer, b.consumer);
        return a.producer_id < b.producer_id;
    }
    void Finalize() {
        std::sort(uses.begin(), uses.end(), Less);
        uses.erase(std::unique(uses.begin(), uses.end(), [](const auto& a, const auto& b) {
            return a.consumer == b.consumer && a.producer_id == b.producer_id;
        }), uses.end());
    }
    [[nodiscard]] bool Discards(const ir::Inst* consumer, u32 producer_id) const {
        if (!consumer || uses.empty()) return false;
        const FlagsOnlySpillUse key{consumer, producer_id};
        const auto found = std::lower_bound(uses.begin(), uses.end(), key, Less);
        return found != uses.end() && found->consumer == consumer && found->producer_id == producer_id;
    }
};
} // namespace swift::runtime::backend::arm64
