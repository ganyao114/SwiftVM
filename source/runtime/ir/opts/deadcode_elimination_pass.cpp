//
// Created by 甘尧 on 2024/6/24.
//

#include "deadcode_elimination_pass.h"

#include <vector>
#include <unordered_map>

namespace swift::runtime::ir {

namespace {

// A guest memory read is architecturally observable even when nobody consumes
// its result: it can fault.  `mov (%rax), %rax` with a wild pointer must raise
// the guest page fault whether or not the loaded value is later overwritten --
// bad_pointer_x86_64 is exactly that program, and it exits 1 (PageFatal) rather
// than reaching its "the load succeeded" exit 42.
//
// Inst::HasSideEffects() reasons from use counts and return types and so calls
// an unused load removable.  Nothing hit that before, because the front end
// always stored a load's result into a guest register and the store held the
// load alive; once dead uniform stores are eliminated the store can disappear
// and the load with it.  The fault, not the value, is the side effect.
[[nodiscard]] bool Removable(Inst& inst) {
    if (inst.GetUses(false) != 0) return false;
    return !inst.HasSideEffects();
}

void Eliminate(const std::vector<Block*>& blocks, HIRFunction* function) {
    std::unordered_map<Inst*, Block*> owners;
    std::vector<Inst*> work;
    for (auto* block : blocks) {
        for (auto& inst : block->GetInstList()) {
            owners.emplace(&inst, block);
            if (Removable(inst)) work.push_back(&inst);
        }
    }
    // FIFO keeps the removal order deterministic.
    for (size_t next = 0; next < work.size(); ++next) {
        auto* inst = work[next];
        auto owner = owners.find(inst);
        // Duplicate operand edges may enqueue the same definition. Test its
        // membership before dereferencing any instruction already deleted.
        if (owner == owners.end() || !Removable(*inst)) continue;
        auto operands = inst->GetValues();
        auto* block = owner->second;
        owners.erase(owner);
        if (function) function->EraseInst(block, inst);
        else block->DestroyInst(inst);
        for (auto value : operands) {
            auto* def = value.Def();
            if (owners.contains(def) && Removable(*def)) work.push_back(def);
        }
    }
}

}  // namespace

void DeadCodeEliminationPass::Run(Block* block, HIRFunction* hir_function) {
    Eliminate({block}, hir_function);
}

void DeadCodeEliminationPass::Run(HIRBuilder* hir_builder) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void DeadCodeEliminationPass::Run(HIRFunction* hir_function) {
    std::vector<Block*> blocks;
    for (auto* hir_block : hir_function->GetHIRBlocks()) {
        if (hir_block) blocks.push_back(hir_block->GetBlock());
    }
    // A shared worklist also removes predecessor defs whose last use dies in
    // a later block, without rescanning the function to reach a fixed point.
    Eliminate(blocks, hir_function);
}

}  // namespace swift::runtime::ir
