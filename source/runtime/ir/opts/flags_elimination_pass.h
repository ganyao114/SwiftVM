//
// Created by 甘尧 on 2024/6/26.
//

#pragma once

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class FlagsEliminationPass {
public:
    static void Run(HIRBuilder *hir_builder);
    static void Run(HIRFunction *hir_function);
    // `hir_function` is non-null only on the function-compilation path: the
    // pass deletes instructions, and in an HIRFunction every deletion has to go
    // through EraseInst so the HIRValue use lists stay consistent.
    static void Run(Block *block, HIRFunction *hir_function = nullptr);
};

}  // namespace swift::runtime::ir
