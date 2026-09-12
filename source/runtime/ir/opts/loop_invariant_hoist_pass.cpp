#include "loop_invariant_hoist_pass.h"

#include <algorithm>

#include "runtime/common/variant_util.h"

namespace swift::runtime::ir {
namespace {

bool TerminalHasSelfEdge(const Terminal& terminal_value, Location self) {
    return VisitVariant<bool>(terminal_value, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, terminal::LinkBlock> ||
                      std::is_same_v<T, terminal::LinkBlockFast>) {
            return term.next == self;
        } else if constexpr (std::is_same_v<T, terminal::If> ||
                             std::is_same_v<T, terminal::Condition>) {
            return TerminalHasSelfEdge(term.then_, self) ||
                   TerminalHasSelfEdge(term.else_, self);
        } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
            return TerminalHasSelfEdge(term.else_, self);
        } else if constexpr (std::is_same_v<T, terminal::Switch>) {
            return std::any_of(term.cases.begin(), term.cases.end(),
                               [&](const auto& item) {
                                   return TerminalHasSelfEdge(item.then, self);
                               });
        }
        return false;
    });
}

bool TerminalHasConditionalSelfEdge(const Terminal& terminal_value, Location self) {
    return VisitVariant<bool>(terminal_value, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, terminal::If> ||
                      std::is_same_v<T, terminal::Condition>) {
            return TerminalHasSelfEdge(term.then_, self) ||
                   TerminalHasSelfEdge(term.else_, self);
        }
        return false;
    });
}

bool Overlaps(u32 left, u32 left_size, u32 right, u32 right_size) {
    return left < right + right_size && right < left + left_size;
}

bool UnitWritesUniform(HIRFunction* function, u32 offset, u32 size) {
    for (auto* hir_block : function->GetHIRBlocks()) {
        for (auto& inst : hir_block->GetInstList()) {
            if (inst.GetOp() != OpCode::StoreUniform) {
                continue;
            }
            const auto uniform = inst.GetArg<Uniform>(0);
            const u32 store_size = GetValueSizeByte(inst.GetArg<Value>(1).Type());
            if (Overlaps(offset, size, uniform.GetOffset(), store_size)) {
                return true;
            }
        }
    }
    return false;
}

bool AllUsesStayInBlock(HIRFunction* function, Block* owner, Inst* definition) {
    bool found = false;
    for (auto* hir_block : function->GetHIRBlocks()) {
        auto* block = hir_block->GetBlock();
        for (auto& inst : block->GetInstList()) {
            for (auto value : inst.GetValues()) {
                if (value.Def() != definition) {
                    continue;
                }
                found = true;
                if (block != owner) {
                    return false;
                }
            }
        }
    }
    return found;
}

bool IsWideLoopCompareConstant(Block* block, Inst* definition) {
    const u64 immediate = definition->GetArg<Imm>(0).Get();
    const u32 nonzero_halfwords =
            static_cast<u32>((immediate & 0xffffu) != 0) +
            static_cast<u32>(((immediate >> 16) & 0xffffu) != 0) +
            static_cast<u32>(((immediate >> 32) & 0xffffu) != 0) +
            static_cast<u32>(((immediate >> 48) & 0xffffu) != 0);
    if (nonzero_halfwords < 2) {
        return false;
    }

    Inst* arithmetic = nullptr;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != OpCode::Add && inst.GetOp() != OpCode::Sub) {
            continue;
        }
        for (auto value : inst.GetValues()) {
            if (value.Def() == definition) {
                if (arithmetic) {
                    return false;
                }
                arithmetic = &inst;
            }
        }
    }
    if (!arithmetic) {
        return false;
    }
    for (auto& inst : block->GetInstList()) {
        if ((inst.GetOp() == OpCode::SaveFlags ||
             inst.GetOp() == OpCode::BranchOnlyFlags) &&
            inst.GetArg<Value>(0).Def() == arithmetic) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::unique_ptr<LoopInvariantHoistRecipe> LoopInvariantHoistRecipe::Analyze(
        HIRFunction* function,
        const UniformInfo& info,
        const FeatureSet& features) {
    auto recipe = std::make_unique<LoopInvariantHoistRecipe>();
    if (!function || (!features.loop_gpr_hoist && !features.loop_const_hoist)) {
        return recipe;
    }

    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        auto* block = hir_block.GetBlock();
        if (!block || !block->HasTerminal() ||
            !TerminalHasSelfEdge(block->GetTerminal(), block->GetStartLocation())) {
            continue;
        }

        BlockRecipe block_recipe{};
        block_recipe.block = block;
        for (auto& inst : block->GetInstList()) {
            block_recipe.original_order.push_back(&inst);
        }
        for (auto* inst : block_recipe.original_order) {
            // 两条及以上长活基址在 STREAM Triad 上虽不 spill，仍触发稳定的
            // Apple 核心短环退化；P0 因而只收一个家，其余保持原按需 load。
            constexpr u16 kMaxGPRAnchors = 1;
            if (features.loop_gpr_hoist &&
                block_recipe.gpr_count < kMaxGPRAnchors &&
                inst->GetOp() == OpCode::LoadUniform &&
                inst->ReturnType() == ValueType::U64) {
                const auto uniform = inst->GetArg<Uniform>(0);
                const u32 offset = uniform.GetOffset();
                constexpr u32 size = sizeof(u64);
                if (info.IsLoopGPRUniformRange(offset, size) &&
                    info.uniform_regs_map.GetValueAt(offset).Null() &&
                    !UnitWritesUniform(function, offset, size) &&
                    AllUsesStayInBlock(function, block, inst)) {
                    block_recipe.anchors.push_back(inst);
                    ++block_recipe.gpr_count;
                    continue;
                }
            }
            if (features.loop_const_hoist && inst->GetOp() == OpCode::LoadImm &&
                inst->ReturnType() == ValueType::U64 &&
                TerminalHasConditionalSelfEdge(block->GetTerminal(),
                                               block->GetStartLocation()) &&
                AllUsesStayInBlock(function, block, inst) &&
                IsWideLoopCompareConstant(block, inst)) {
                block_recipe.anchors.push_back(inst);
                ++block_recipe.const_count;
            }
        }
        if (!block_recipe.anchors.empty()) {
            std::sort(block_recipe.anchors.begin(), block_recipe.anchors.end(),
                      [](const Inst* left, const Inst* right) {
                          return left->Id() < right->Id();
                      });
            recipe->blocks.push_back(std::move(block_recipe));
        }
    }
    return recipe;
}

void LoopInvariantHoistRecipe::Apply() {
    ASSERT(!applied);
    for (auto& block_recipe : blocks) {
        auto* block = block_recipe.block;
        for (auto* anchor : block_recipe.anchors) {
            block->RemoveInst(anchor);
        }
        for (auto it = block_recipe.anchors.rbegin(); it != block_recipe.anchors.rend(); ++it) {
            block->InsertBefore(*it, block->GetBeginInst().operator->());
        }
        block->SetLoopHoistMetadata({block_recipe.anchors.back(),
                                     std::span<Inst*>{block_recipe.anchors},
                                     block_recipe.gpr_count,
                                     block_recipe.const_count});
    }
    applied = true;
}

void LoopInvariantHoistRecipe::Revert() {
    ASSERT(applied);
    for (auto& block_recipe : blocks) {
        auto* block = block_recipe.block;
        std::vector<Inst*> current;
        current.reserve(block_recipe.original_order.size());
        for (auto& inst : block->GetInstList()) {
            current.push_back(&inst);
        }
        for (auto* inst : current) {
            block->RemoveInst(inst);
        }
        for (auto* inst : block_recipe.original_order) {
            block->AppendInst(inst);
        }
        block->SetLoopHoistMetadata({});
    }
    applied = false;
}

}  // namespace swift::runtime::ir
