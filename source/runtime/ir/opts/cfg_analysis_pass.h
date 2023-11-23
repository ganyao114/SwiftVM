//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class CFGAnalysisPass {
public:
    static void Run(HIRBuilder *hir_builder);
    static void Run(HIRFunction *hir_function);

private:
    static bool UpdateDominatorOfSuccessor(HIRBlock* block, HIRBlock* successor);

    static void FindDominateEdges(HIRFunction *hir_function);
    static void FindBackEdges(HIRFunction *hir_function, BitVector &visited);
    static void ComputeDominanceInformation(HIRFunction *hir_function);
};

}  // namespace swift::runtime::ir