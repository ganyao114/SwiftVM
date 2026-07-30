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
    static void RunWithScalarInsert(HIRFunction *hir_function,
                                    backend::RegAlloc *reg_alloc,
                                    bool scalar_insert);
    // Explicit selector used by the equivalence tests to run both algorithms
    // in one process. Production callers use the overload above, which reads
    // SVM_RA_1BLK once and defaults to the fast path.
    static void Run(HIRFunction *hir_function,
                    backend::RegAlloc *reg_alloc,
                    bool single_block_fast_path);
    static void Run(ir::Block *block,
                    backend::RegAlloc *reg_alloc,
                    bool scalar_insert = false);
};

class VRegisterAllocPass {
public:
    static void Run(ir::Block *block);
};

}  // namespace swift::runtime::ir
