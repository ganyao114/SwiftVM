//
// Created by 甘尧 on 2024/6/24.
//

#include "deadcode_elimination_pass.h"

#include <vector>

namespace swift::runtime::ir {

void DeadCodeEliminationPass::Run(Block* block) {
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
            if (!it->HasSideEffects()) {
                it = inst_list.erase(it);
                delete pre.operator->();
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
        Run(hir_block.GetBlock());
    }
}

}  // namespace swift::runtime::ir
