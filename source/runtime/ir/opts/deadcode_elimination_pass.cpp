//
// Created by 甘尧 on 2024/6/24.
//

#include "deadcode_elimination_pass.h"

#include <vector>

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
    switch (inst.GetOp()) {
        case OpCode::LoadMemory:
        case OpCode::LoadMemoryTSO:
            return false;
        default:
            return !inst.HasSideEffects();
    }
}

}  // namespace

void DeadCodeEliminationPass::Run(Block* block, HIRFunction* hir_function) {
    auto& inst_list = block->GetInstList();
    // Iterate to a fixpoint: removing an instruction can kill the last use of
    // an EARLIER def (e.g. once FlagEliminationPass drops a dead SaveFlags,
    // the flag-only def chain behind it dies link by link). A single forward
    // sweep would leave those corpses in the list and the backends would try
    // to emit instructions the register allocator never gave a slot.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = inst_list.begin(); it != inst_list.end();) {
            auto pre = it;
            if (Removable(*it)) {
                if (hir_function) {
                    // EraseInst unlinks from inst_list itself; step past the
                    // victim before it is freed.
                    ++it;
                    hir_function->EraseInst(block, pre.operator->());
                } else {
                    it = inst_list.erase(it);
                    delete pre.operator->();
                }
                changed = true;
            } else {
                ++it;
            }
        }
    }
}

void DeadCodeEliminationPass::Run(HIRBuilder* hir_builder) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void DeadCodeEliminationPass::Run(HIRFunction* hir_function) {
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        Run(hir_block.GetBlock(), hir_function);
    }
}

}  // namespace swift::runtime::ir
