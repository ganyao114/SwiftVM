//
// Created by 甘尧 on 2024/6/24.
//

#include "deadcode_elimination_pass.h"

namespace swift::runtime::ir {

void DeadCodeEliminationPass::Run(Block* block) {
    auto &inst_list = block->GetInstList();
    for (auto it = inst_list.begin(); it != inst_list.end();) {
        auto pre = it;
        if (!it->HasSideEffects()) {
            it = inst_list.erase(it);
            delete pre.operator->();
        } else {
            it++;
        }
    }
}

void DeadCodeEliminationPass::Run(HIRBuilder* hir_builder) {

}

}  // namespace swift::runtime::ir
