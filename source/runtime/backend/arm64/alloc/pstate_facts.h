#pragma once

#include <optional>
#include <unordered_map>
#include "runtime/backend/arm64/jit/block_analysis_index.h"

namespace swift::runtime::backend::arm64 {
struct PstateFacts {
    std::unordered_map<const ir::Inst*, ir::Flags> available;
    std::optional<u32> first_advance;
    ir::Flags At(const ir::Inst* test) const {
        const auto found = available.find(test);
        return found == available.end() ? ir::Flags::None : found->second;
    }
};
// Static linear-prefix eligibility. Actual NZCV validity remains emitter state.
PstateFacts BuildPstateFacts(const BlockAnalysisIndex& analysis);
}
