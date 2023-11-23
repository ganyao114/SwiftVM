//
// Created by 甘尧 on 2023/9/16.
//

#include "dataflow_analysis_pass.h"

namespace swift::runtime::ir {

void DataflowAnalysisPass::Run(HIRBuilder* hir_builder) {
    for (auto &hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void DataflowAnalysisPass::Run(HIRFunction* hir_function) {
    auto &hir_blocks = hir_function->GetHIRBlocks();
    StackVector<BitVector, 16> incoming_bitvectors{hir_function->MaxBlockCount()};
    for (auto hir_block : hir_blocks) {
        auto &values = hir_block->GetHIRValues();
        incoming_bitvectors[hir_block->GetOrderId()] = BitVector{values.size()};
    }
}

}  // namespace swift::runtime::ir
