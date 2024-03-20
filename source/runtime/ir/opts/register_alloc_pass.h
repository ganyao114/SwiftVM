//
// Created by 甘尧 on 2023/12/6.
//

#pragma once

#include "runtime/ir/hir_builder.h"
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::ir {

class RegisterAllocPass {
public:
    static void Run(HIRBuilder *hir_builder, backend::RegAlloc *reg_alloc);
    static void Run(HIRFunction *hir_function, backend::RegAlloc *reg_alloc);
};

class VRegisterAllocPass {
public:
    static void Run(ir::Block *block);
};

}  // namespace swift::runtime::ir
