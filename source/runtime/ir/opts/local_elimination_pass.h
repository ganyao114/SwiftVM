//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class LocalEliminationPass {
public:
    static void Run(HIRBuilder *hir_builder, bool mem_to_regs = true);
    static void Run(HIRFunction *hir_function, bool mem_to_regs = true);
    static void Run(Block *block);
};

}  // namespace swift::runtime::ir