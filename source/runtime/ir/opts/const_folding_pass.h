//
// Created by 甘尧 on 2023/12/6.
//

#pragma once

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class ConstFoldingPass {
public:
    static void Run(HIRBuilder *hir_builder);
    static void Run(HIRFunction *hir_function);
    static void Run(Block *block);
};

}  // namespace swift::runtime::ir