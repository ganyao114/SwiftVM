//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class DataflowAnalysisPass {
public:
    static void Run(HIRBuilder *hir_builder);
    static void Run(HIRFunction *hir_function);
};

}  // namespace swift::runtime::ir