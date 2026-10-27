#pragma once

#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
}
namespace swift::runtime::backend::arm64 {

// A caller-owned projection. Observer/full-write policies differ between
// preallocation and publication, while both read the same canonical uses.
class GPRWriteSuccessors final {
public:
    template <class IsObserver, class IsFullWrite>
    void Build(const compile::BlockAnalysisIndex& facts,
               IsObserver observes,
               IsFullWrite full_write) {
        index = &facts;
        following.clear();
        dense.clear();
        if (facts.DenseInstructionIDs()) dense.assign(facts.Instructions().size(), nullptr);
        std::array<const ir::Inst*, 32> next{};
        for (auto* inst : std::views::reverse(facts.Instructions())) {
            const auto op = inst->GetOp();
            if (op == ir::OpCode::SetHostGPR) {
                const auto home = inst->GetArg<ir::Imm>(1).Get();
                if (home < next.size() && next[home]) {
                    if (const auto slot = facts.InstructionSlot(inst))
                        dense[*slot] = next[home];
                    else
                        following.emplace(inst, next[home]);
                }
            }
            if (observes(*inst)) {
                next.fill(nullptr);
            } else if (op == ir::OpCode::GetHostGPR) {
                const auto home = inst->GetArg<ir::Imm>(0).Get();
                if (home < next.size()) next[home] = nullptr;
            } else if (op == ir::OpCode::SetHostGPR) {
                const auto home = inst->GetArg<ir::Imm>(1).Get();
                if (home < next.size()) next[home] = full_write(*inst) ? inst : nullptr;
            }
        }
    }

    [[nodiscard]] const ir::Inst* FollowingWrite(const ir::Inst* write) const {
        ASSERT(index);
        if (const auto slot = index->InstructionSlot(write)) return dense[*slot];
        const auto found = following.find(write);
        return found == following.end() ? nullptr : found->second;
    }

private:
    const compile::BlockAnalysisIndex* index{};
    std::vector<const ir::Inst*> dense;
    std::unordered_map<const ir::Inst*, const ir::Inst*> following;
};

}  // namespace swift::runtime::backend::arm64
