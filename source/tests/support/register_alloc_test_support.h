#pragma once
namespace swift::runtime::backend::arm64 {
class ResidencePreparation;
class AllocationPreparation;
}  // namespace swift::runtime::backend::arm64
#include <vector>
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/allocate/inputs.h"
#include "runtime/compile/allocate/writer.h"
#include "runtime/ir/hir_builder.h"
namespace swift::runtime::ir {
// Test-only CFG edits and raw allocation ownership checks. Keep edits in the
// test target, and respect the same freeze boundary as the public builders.
class HIRTestSupport {
public:
    static void InitializeParamsNodeSlab(size_t capacity) {
        Params::Node::InitializeSlabHeap(capacity);
    }
    static void* AllocateParamsNodeSlot() { return Params::Node::Allocate(); }
    static void FreeParamsNodeSlot(void* slot) {
        Params::Node::Free(static_cast<Params::Node*>(slot));
    }
    static void SetStartLocation(AddressNode* node, Location location) {
        node->CheckRangeMutable();
        node->location = location;
    }
    static void SetMember(HIRFunction* function, size_t index, const HIRBlock* block) {
        function->CheckMutable();
        ASSERT(index < function->blocks.size());
        const auto* previous = function->blocks[index];
        ASSERT(!previous || !previous->GetBlock()->IsIRReadOnly());
        ASSERT(!block || !block->GetBlock()->IsIRReadOnly());
        function->blocks[index] = const_cast<HIRBlock*>(block);
    }
    static void SetRPO(HIRFunction* function, HIRBlockView order) {
        function->CheckMutable();
        for (auto* block : order) ASSERT(block && !block->GetBlock()->IsIRReadOnly());
        function->blocks_rpo.clear();
        for (auto* block : order) function->blocks_rpo.push_back(*const_cast<HIRBlock*>(block));
    }
    static void SetPredecessor(const HIRBlock* block, size_t index, const HIRBlock* predecessor) {
        ASSERT(block && !block->GetBlock()->IsIRReadOnly());
        ASSERT(index < block->predecessors.size());
        const_cast<HIRBlock*>(block)->predecessors[index] = predecessor;
    }
    static void SetSuccessor(const HIRBlock* block, size_t index, const HIRBlock* successor) {
        ASSERT(block && !block->GetBlock()->IsIRReadOnly());
        ASSERT(index < block->successors.size());
        const_cast<HIRBlock*>(block)->successors[index] = successor;
    }
    static void InsertForeignEdge(HIRFunction* function,
                                  const HIRBlock* source,
                                  const HIRBlock* target);
};

class RegisterAllocTestSupport {
public:
    // Standalone target tests finish through the real preparation submissions.
    static void FinishGPRForTest(backend::arm64::ResidencePreparation&,
                                 backend::arm64::AllocationPreparation&,
                                 const FeatureSet&);
    static void SwapConsumerActionsForAdmissionTest(const backend::arm64::UnitActions&,
                                                    std::vector<backend::arm64::ConsumerAction>&,
                                                    u32& revision);
    using SpillEvictTestResult = compile::AllocationStatistics;
    // Test-only overrides for comparisons within one process.
    // Runtime callers use the compile allocation entries and the resolved feature settings.
    static void RunForIntWidthTieTest(ir::Block* block,
                                      compile::AllocationWriter* reg_alloc,
                                      bool intwidth_tie);
    static void RunForCoalesceTest(ir::Block* block,
                                   compile::AllocationWriter* reg_alloc,
                                   bool coalesce);
    static void RunForCoalesceLiveTest(ir::Block* block,
                                       compile::AllocationWriter* reg_alloc,
                                       bool live);
    static void RunForCoalesceConflictTest(ir::Block* block,
                                           compile::AllocationWriter* reg_alloc,
                                           u32 tied_value_id,
                                           u16 target);
    static void RunForPinnedWConflictTest(ir::Block* block,
                                          compile::AllocationWriter* reg_alloc,
                                          const std::vector<u32>& tied_value_ids,
                                          const std::vector<u32>& write_coalesced_ids,
                                          u16 target);
    static void RunForWidthChainTest(ir::Block* block,
                                     compile::AllocationWriter* reg_alloc,
                                     bool enabled);
    static void RunForWidthChainConflictTest(ir::Block* block,
                                             compile::AllocationWriter* reg_alloc,
                                             u32 tied_value_id,
                                             u16 target);
    static void RunForWidthChainLongConflictTest(ir::Block* block,
                                                 compile::AllocationWriter* reg_alloc,
                                                 u32 tied_value_id,
                                                 u16 target);
    static void RunForXmmResidentTest(ir::Block* block,
                                      compile::AllocationWriter* reg_alloc,
                                      bool enabled);
    static void RunForXmmResidentConflictTest(ir::Block* block,
                                              compile::AllocationWriter* reg_alloc,
                                              u32 tied_value_id,
                                              u16 target);
    static void RunForAesChainTieTest(ir::Block* block,
                                      compile::AllocationWriter* reg_alloc,
                                      bool enabled);
    static void RunForAesChainTieConflictTest(ir::Block* block,
                                              compile::AllocationWriter* reg_alloc,
                                              u32 tied_value_id,
                                              u16 target);
    static void RunForScalarFPRTieTest(ir::Block* block,
                                       compile::AllocationWriter* reg_alloc,
                                       bool enabled);
    static void RunForScalarFPRTieConflictTest(ir::Block* block,
                                               compile::AllocationWriter* reg_alloc,
                                               u32 tied_value_id,
                                               u16 target);
    static void RunForShufpsImmTieTest(ir::Block* block,
                                       compile::AllocationWriter* reg_alloc,
                                       bool enabled);
    static void BuildSpillReloadsForTest(const ir::Block*,
                                         compile::AllocationWriter*,
                                         const compile::AllocationInputs&);
    static SpillEvictTestResult RunForSpillEvictTest(ir::Block* block,
                                                     compile::AllocationWriter* reg_alloc,
                                                     bool spill_evict);
    static SpillEvictTestResult RunForSpillEvictTest(ir::Block* block,
                                                     compile::AllocationWriter* reg_alloc,
                                                     compile::AllocationInputs inputs);
};
}  // namespace swift::runtime::ir
