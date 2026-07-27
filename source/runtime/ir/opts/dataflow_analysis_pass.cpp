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

// This pass has no implementation and no callers anywhere in the tree --
// PassPipeline never registers it and nothing else calls Run(). The body used
// to allocate one
// BitVector per HIR block, size them, and return without reading any of them:
// pure allocation churn on a path that, had it ever been wired in, would have
// run once per compiled unit. The scaffolding is kept (the declaration is the
// record of the intended pass) but it no longer allocates.
//
// If dataflow analysis is implemented here later, note the shape the old body
// assumed and did not check: it indexed `incoming_bitvectors` by
// HIRBlock::GetOrderId() while sizing the vector from MaxBlockCount(). That
// holds only as long as order ids stay dense over [0, MaxBlockCount()), which
// is an invariant of the RPO numbering, not of the block list -- assert it
// rather than inherit it.
void DataflowAnalysisPass::Run(HIRFunction* hir_function) {
    (void)hir_function;
}

}  // namespace swift::runtime::ir
