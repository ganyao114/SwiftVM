//
// Created by 甘尧 on 2023/12/6.
//

#pragma once

#include "runtime/ir/hir_builder.h"
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::ir {

class RegisterAllocPass {
public:
    struct SpillEvictTestResult {
        u32 eviction_restarts{};
        u32 final_gpr_reserve{};
        u32 final_fpr_reserve{};
        bool fell_back_to_ladder{};
    };

    static void Run(HIRBuilder *hir_builder, backend::RegAlloc *reg_alloc,
                    const FeatureSet& features);
    static void Run(HIRFunction *hir_function, backend::RegAlloc *reg_alloc,
                    const FeatureSet& features);
    static void RunWithScalarInsert(HIRFunction *hir_function,
                                    backend::RegAlloc *reg_alloc,
                                    bool scalar_insert,
                                    const FeatureSet& features);
    // Explicit selector used by the equivalence tests to run both algorithms
    // in one process. Production callers use the overload above, which reads
    // SVM_RA_1BLK once and defaults to the fast path.
    static void Run(HIRFunction *hir_function,
                    backend::RegAlloc *reg_alloc,
                    bool single_block_fast_path,
                    const FeatureSet& features);
    static void Run(ir::Block *block,
                    backend::RegAlloc *reg_alloc,
                    bool scalar_insert,
                    const FeatureSet& features);
    // Explicit gate selector for unit tests that compare OFF and ON allocation
    // in one process. Production callers use the overload above, which reads
    // SVM_RA_INTWIDTH_TIE once and defaults to OFF.
    static void RunForIntWidthTieTest(ir::Block *block,
                                     backend::RegAlloc *reg_alloc,
                                     bool intwidth_tie);
    static void RunForInductTieTest(ir::Block *block,
                                    backend::RegAlloc *reg_alloc,
                                    bool induct_tie);
    static void RunForCoalesceTest(ir::Block *block,
                                   backend::RegAlloc *reg_alloc,
                                   bool coalesce);
    static void RunForCoalesceConflictTest(ir::Block *block,
                                           backend::RegAlloc *reg_alloc,
                                           u32 tied_value_id,
                                           u16 target);
    static void RunForWidthChainTest(ir::Block *block,
                                     backend::RegAlloc *reg_alloc,
                                     bool enabled);
    static void RunForWidthChainConflictTest(ir::Block *block,
                                             backend::RegAlloc *reg_alloc,
                                             u32 tied_value_id,
                                             u16 target);
    static void RunForWidthChainLongConflictTest(ir::Block *block,
                                                 backend::RegAlloc *reg_alloc,
                                                 u32 tied_value_id,
                                                 u16 target);
    static void RunForXmmResidentTest(ir::Block *block,
                                      backend::RegAlloc *reg_alloc,
                                      bool enabled);
    static void RunForXmmResidentConflictTest(ir::Block *block,
                                              backend::RegAlloc *reg_alloc,
                                              u32 tied_value_id,
                                              u16 target);
    static void RunForScalarFPRTieTest(ir::Block *block,
                                       backend::RegAlloc *reg_alloc,
                                       bool enabled);
    static void RunForScalarFPRTieConflictTest(ir::Block *block,
                                               backend::RegAlloc *reg_alloc,
                                               u32 tied_value_id,
                                               u16 target);
    static void RunForShufpsImmTieTest(ir::Block *block,
                                       backend::RegAlloc *reg_alloc,
                                       bool enabled);
    static SpillEvictTestResult RunForSpillEvictTest(
            ir::Block *block,
            backend::RegAlloc *reg_alloc,
            bool spill_evict);
};

class VRegisterAllocPass {
public:
    static void Run(ir::Block *block);
};

}  // namespace swift::runtime::ir
