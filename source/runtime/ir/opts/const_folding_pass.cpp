//
// Created by 甘尧 on 2023/12/6.
//

#include "const_folding_pass.h"

namespace swift::runtime::ir {

void ConstFoldingPass::Run(HIRBuilder* hir_builder) {
    for (auto &hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void ConstFoldingPass::Run(HIRFunction* hir_function) {

}

void ConstFoldingPass::Run(Block* block) {

}

}
