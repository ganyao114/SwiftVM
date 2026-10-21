#pragma once

#include <vector>
#include <map>
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::arm64 {
struct GPRPublicationAction;
// A retained PF result must stay in this physical home through the block.
// Actual token validity, publication and materialization remain emitter state.
struct FlagsTokenPlan {
    u32 first_id{};
    std::vector<u32> homes;
    std::map<std::pair<u32, u32>, u32> point_homes;
    bool point_dependent{};
    [[nodiscard]] bool Retains(const ir::Inst* producer, u32 home, u32 point) const {
        if (home >= 31) return false;
        if (point_dependent) {
            if (!producer) return false;
            const auto found = point_homes.find({producer->Id(), point});
            return found != point_homes.end() && (found->second & (1u << home));
        }
        return producer && producer->Id() >= first_id &&
               producer->Id() - first_id < homes.size() &&
               (homes[producer->Id() - first_id] & (1u << home));
    }
};
FlagsTokenPlan BuildFlagsTokenPlan(const BlockAnalysisIndex& analysis,
        const RegAlloc& allocation, const FeatureSet& features,
        const std::map<ir::Inst*, GPRPublicationAction>* publications = nullptr,
        const PseudoFlagsPlan* demands = nullptr);
}
