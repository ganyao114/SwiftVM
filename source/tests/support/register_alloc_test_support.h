#pragma once
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/opts/allocation_statistics.h"
namespace swift::runtime::ir {
class RegisterAllocTestSupport {
public:
    using SpillEvictTestResult = AllocationStatistics;
    // Test-only overrides for comparisons within one process.
    // Runtime callers use RegisterAllocPass and the resolved feature settings.
    static void RunForIntWidthTieTest(ir::Block *block,
                                     backend::RegAlloc *reg_alloc,
                                     bool intwidth_tie);
    static void RunForCoalesceTest(ir::Block *block,
                                   backend::RegAlloc *reg_alloc,
                                   bool coalesce);
    static void RunForCoalesceLiveTest(ir::Block *block,
                                       backend::RegAlloc *reg_alloc,
                                       bool live);
    static void RunForCoalesceConflictTest(ir::Block *block,
                                           backend::RegAlloc *reg_alloc,
                                           u32 tied_value_id,
                                           u16 target);
    static void RunForPinnedWConflictTest(ir::Block *block,
                                          backend::RegAlloc *reg_alloc,
                                          const std::vector<u32>& tied_value_ids,
                                          const std::vector<u32>& write_coalesced_ids,
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
    static void RunForAesChainTieTest(ir::Block *block,
                                      backend::RegAlloc *reg_alloc,
                                      bool enabled);
    static void RunForAesChainTieConflictTest(ir::Block *block,
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
}
