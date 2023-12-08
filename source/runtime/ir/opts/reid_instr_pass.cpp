//
// Created by 甘尧 on 2023/12/6.
//

#include "reid_instr_pass.h"

namespace swift::runtime::ir {

void ReIdInstrPass::Run(HIRBuilder* hir_builder) {
    for (auto &hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void ReIdInstrPass::Run(HIRFunction* hir_function) {
    hir_function->IdByRPO();
}

}
