#pragma once

#include <array>
#include <ranges>
#include <span>
#include <unordered_map>
#include <vector>
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

// Built once after IR/RA are final. Pointers, order and operand multiplicities
// remain valid through emission; rebuild when entering another block.
class BlockAnalysisIndex {
public:
    struct Use {
        ir::Inst* consumer;
        u32 count;
    };

    void Build(ir::Block* block) {
        instructions.clear();
        uses.clear();
        following_writes.clear();
        for (auto& inst : block->GetInstList()) {
            instructions.push_back(&inst);
            for (auto value : inst.GetValues()) {
                if (!value.Def()) continue;
                auto& consumers = uses[value.Def()];
                if (!consumers.empty() && consumers.back().consumer == &inst) {
                    ++consumers.back().count;
                } else {
                    consumers.push_back({&inst, 1});
                }
            }
        }
    }

    std::span<const Use> Uses(ir::Inst* definition) const {
        const auto it = uses.find(definition);
        return it == uses.end() ? std::span<const Use>{} : it->second;
    }

    std::span<ir::Inst* const> Instructions() const { return instructions; }

    template<class IsObserver, class IsFullWrite>
    void BuildWriteSuccessors(IsObserver observes, IsFullWrite full_write) {
        following_writes.clear();
        std::array<ir::Inst*, 32> next{};
        for (auto* inst : std::views::reverse(instructions)) {
            const auto op = inst->GetOp();
            if (op == ir::OpCode::SetHostGPR) {
                const auto home = inst->GetArg<ir::Imm>(1).Get();
                if (home < next.size() && next[home]) {
                    following_writes.emplace(inst, next[home]);
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

    ir::Inst* FollowingWrite(ir::Inst* write) const {
        const auto it = following_writes.find(write);
        return it == following_writes.end() ? nullptr : it->second;
    }

private:
    std::vector<ir::Inst*> instructions;
    std::unordered_map<ir::Inst*, std::vector<Use>> uses;
    std::unordered_map<ir::Inst*, ir::Inst*> following_writes;
};

} // namespace swift::runtime::backend::arm64
