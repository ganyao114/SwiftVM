#pragma once

#include <memory>
#include <unordered_map>

#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

// Per-unit shared analysis facts (middle-pipeline redesign, step 1).
// One instance per allocation attempt, owned by RegAlloc, so a cached
// index always describes exactly the IR its attempt allocated:
// baseline and hoisted attempts own distinct RegAlloc objects, and no
// stage after the IR opt pipeline mutates IR. Block use facts are
// built once here instead of once per consuming phase.
class UnitFacts {
public:
    // Full use facts for `block`, built on first request. The build
    // collects pseudo-flag demand as well, so one build serves both
    // fixed-GPR constraint preparation and target planning.
    // Write-successor data is not part of the shared contract: every
    // consumer rebuilds it with its own observer/full-write
    // predicates before reading FollowingWrite().
    [[nodiscard]] BlockAnalysisIndex& Uses(ir::Block* block) {
        auto& slot = blocks[block];
        if (!slot) {
            slot = std::make_unique<BlockAnalysisIndex>();
            slot->Build(block, nullptr, true);
        }
        return *slot;
    }

    // Already-built facts without triggering a build.
    [[nodiscard]] const BlockAnalysisIndex* FindUses(const ir::Block* block) const {
        const auto found = blocks.find(block);
        return found == blocks.end() ? nullptr : found->second.get();
    }

private:
    std::unordered_map<const ir::Block*, std::unique_ptr<BlockAnalysisIndex>> blocks{};
};

}  // namespace swift::runtime::backend::arm64
