#include "base/scope_exit.h"
#include "register_alloc_test_support.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/compile/fixed_validation.h"
#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/residence_preparation.h"
#include "runtime/backend/arm64/compile/scratch_preparation.h"
#include "runtime/compile/allocate/internal.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/ir/hir_pools.h"
namespace swift::runtime::ir {
void RegisterAllocTestSupport::FinishGPRForTest(backend::arm64::ResidencePreparation& builder,
                                                backend::arm64::AllocationPreparation& allocation,
                                                const FeatureSet& features) {
    ASSERT(allocation.TargetPlanning());
    auto rollback = ScopeExit([&] { allocation.AbortTargetPlanning(); });
    auto& index = builder.PrepareUseFacts();
    auto draft = builder.FinishConstraints();
    const auto* block = allocation.InputBlock();
    ASSERT(block);
    backend::arm64::FixedValidationDescription described{block, allocation, features, index};
    compile::VerifyFixedConstraints(block, allocation, described.Inputs(), draft.writes);
    allocation.DeclareBlock(block, index.Summary(), index.PseudoFlags().View());
    allocation.StoreGPRResidences(block, std::move(draft.residences));
    allocation.StoreScratch(block,
                            backend::arm64::PrepareScratch(block,
                                                           allocation,
                                                           features,
                                                           backend::arm64::PrepareUnitScratch(
                                                                   allocation.GetGprs(), features),
                                                           index,
                                                           allocation.Workspace()));
    allocation.EndTargetPlanning();
    rollback.Cancel();
}
void RegisterAllocTestSupport::SwapConsumerActionsForAdmissionTest(
        const backend::arm64::UnitActions& actions,
        std::vector<backend::arm64::ConsumerAction>& rows,
        u32& revision) {
    auto& mutable_actions = const_cast<backend::arm64::UnitActions&>(actions);
    mutable_actions.consumers.swap(rows);
    std::swap(mutable_actions.consumer_revision, revision);
}
void HIRTestSupport::InsertForeignEdge(HIRFunction* function,
                                       const HIRBlock* source,
                                       const HIRBlock* target) {
    function->CheckMutable();
    ASSERT(source && target && !source->GetBlock()->IsIRReadOnly() &&
           !target->GetBlock()->IsIRReadOnly());
    auto* edge = function->pools.edges.Create(source, target);
    const_cast<HIRBlock*>(source)->outgoing_edges.push_back(*edge);
    const_cast<HIRBlock*>(target)->incoming_edges.push_back(*edge);
}

using compile::detail::LinearScan;
using compile::detail::AllocateVerified;

// Test entry points share the allocator's internal implementation without
// adding policy branches to the production entry points.

void RegisterAllocTestSupport::RunForIntWidthTieTest(ir::Block* block,
                                                     compile::AllocationWriter* reg_alloc,
                                                     bool intwidth_tie) {
    auto features = FeatureSet{};
    features.ra_intwidth_tie = intwidth_tie;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     intwidth_tie,
                     features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForCoalesceTest(ir::Block* block,
                                                  compile::AllocationWriter* reg_alloc,
                                                  bool coalesce) {
    auto features = FeatureSet{};
    features.ra_coalesce = coalesce;
    features.ra_coalesce_live = false;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     features.ra_intwidth_tie,
                     features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForCoalesceLiveTest(ir::Block* block,
                                                      compile::AllocationWriter* reg_alloc,
                                                      bool live) {
    auto features = FeatureSet{};
    features.ra_coalesce = true;
    features.ra_coalesce_live = live;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     features.ra_intwidth_tie,
                     features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForCoalesceConflictTest(ir::Block* block,
                                                          compile::AllocationWriter* reg_alloc,
                                                          u32 tied_value_id,
                                                          u16 target) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    // The conflict binding below must be installed before the coalescer's
    // only run; keep the initial verified pass from coalescing on its own
    // regardless of the FeatureSet default.
    features.ra_coalesce = false;
    features.ra_coalesce_live = false;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     features.ra_intwidth_tie,
                     features.ra_spill_evict);
    // Model the output of any earlier fixed-home tie independently of its
    // current opcode-specific matcher. The coalescer must treat that mapping
    // as authoritative and reject an intersecting publication window.
    reg_alloc->MapRegister(tied_value_id, HostGPR{target});
    features.ra_coalesce = true;
    auto& facts = reg_alloc->PrepareFacts();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {},
                    {.gpr_reserve = 0, .fpr_reserve = 0},
                    facts,
                    workspace,
                    input_scan};
    scan.CoalesceGuestRegisterAccesses();
}

void RegisterAllocTestSupport::RunForPinnedWConflictTest(
        ir::Block* block,
        compile::AllocationWriter* reg_alloc,
        const std::vector<u32>& tied_value_ids,
        const std::vector<u32>& write_coalesced_ids,
        u16 target) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    features.ra_coalesce = false;
    features.ra_coalesce_live = false;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     features.ra_intwidth_tie,
                     features.ra_spill_evict);
    // Model the homes the linear scan and the write coalescer are proven
    // to have produced for the real-guest shape this targets: the pinned
    // publication chain and an independent fixed read all resident on the
    // same home.  The pinned W-view matcher must treat those mappings as
    // authoritative and reject an intersecting bridge window.
    for (auto id : tied_value_ids) {
        reg_alloc->MapRegister(id, HostGPR{target});
    }
    for (auto id : write_coalesced_ids) {
        reg_alloc->MarkHostWriteCoalesced(id);
    }
    features.ra_coalesce = true;
    auto& facts = reg_alloc->PrepareFacts();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {},
                    {.gpr_reserve = 0, .fpr_reserve = 0},
                    facts,
                    workspace,
                    input_scan};
    scan.CoalesceGuestRegisterAccesses();
}

void RegisterAllocTestSupport::RunForWidthChainTest(ir::Block* block,
                                                    compile::AllocationWriter* reg_alloc,
                                                    bool enabled) {
    auto features = FeatureSet{};
    features.ra_width_chain = enabled;
    features.ra_width_chain_long = false;
    features.ra_coalesce = false;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     features.ra_intwidth_tie,
                     features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForWidthChainConflictTest(ir::Block* block,
                                                            compile::AllocationWriter* reg_alloc,
                                                            u32 tied_value_id,
                                                            u16 target) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    features.ra_width_chain = false;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     features.ra_intwidth_tie,
                     features.ra_spill_evict);
    reg_alloc->MapRegister(tied_value_id, HostGPR{target});
    features.ra_width_chain = true;
    auto& facts = reg_alloc->PrepareFacts();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {},
                    {.gpr_reserve = 0, .fpr_reserve = 0},
                    facts,
                    workspace,
                    input_scan};
    scan.CoalesceWidthChains();
}

void RegisterAllocTestSupport::RunForWidthChainLongConflictTest(
        ir::Block* block, compile::AllocationWriter* reg_alloc, u32 tied_value_id, u16 target) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    features.ra_width_chain = false;
    features.ra_width_chain_long = false;
    AllocateVerified(block,
                     reg_alloc,
                     backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features),
                     false,
                     false,
                     features.ra_intwidth_tie,
                     features.ra_spill_evict);
    reg_alloc->MapRegister(tied_value_id, HostGPR{target});
    features.ra_width_chain_long = true;
    auto& facts = reg_alloc->PrepareFacts();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {},
                    {.gpr_reserve = 0, .fpr_reserve = 0},
                    facts,
                    workspace,
                    input_scan};
    scan.CoalesceWidthChains();
}

void RegisterAllocTestSupport::RunForXmmResidentTest(ir::Block* block,
                                                     compile::AllocationWriter* reg_alloc,
                                                     bool enabled) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    reg_alloc->SetAllocationUnit(block, nullptr);
    auto& facts = reg_alloc->PrepareFacts();
    reg_alloc->ResetAllocations();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {.xmm_resident = enabled},
                    {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR},
                    facts,
                    workspace,
                    input_scan};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocTestSupport::RunForXmmResidentConflictTest(ir::Block* block,
                                                             compile::AllocationWriter* reg_alloc,
                                                             u32 tied_value_id,
                                                             u16 target) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    reg_alloc->SetAllocationUnit(block, nullptr);
    auto& facts = reg_alloc->PrepareFacts();
    reg_alloc->ResetAllocations();
    const auto input_initial = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan initial{block,
                       reg_alloc,
                       {.xmm_resident = false},
                       {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR},
                       facts,
                       workspace,
                       input_initial};
    initial.AllocateRegisters();
    ASSERT(initial.Verify());
    reg_alloc->MapRegister(tied_value_id, HostFPR{target});
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {.xmm_resident = false},
                    {.gpr_reserve = 0, .fpr_reserve = 0},
                    facts,
                    workspace,
                    input_scan};
    scan.CoalesceFPRPublications(true);
}

void RegisterAllocTestSupport::RunForAesChainTieTest(ir::Block* block,
                                                     compile::AllocationWriter* reg_alloc,
                                                     bool enabled) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    features.ra_aes_chain_tie = enabled;
    reg_alloc->SetAllocationUnit(block, nullptr);
    auto& facts = reg_alloc->PrepareFacts();
    reg_alloc->ResetAllocations();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {.xmm_resident = true},
                    {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR},
                    facts,
                    workspace,
                    input_scan};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocTestSupport::RunForAesChainTieConflictTest(ir::Block* block,
                                                             compile::AllocationWriter* reg_alloc,
                                                             u32 tied_value_id,
                                                             u16 target) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    // 初始分配必须显式关闭 tie:本用例的语义是"先无 tie 分配、再强压冲突、
    // 最后开 tie 合并验证整组件拒绝";跟随编译期默认会在第一相就打上
    // tie 标记,而第二相的拒绝无法回滚既有标记,翻盘后测试恒假。
    features.ra_aes_chain_tie = false;
    reg_alloc->SetAllocationUnit(block, nullptr);
    auto& facts = reg_alloc->PrepareFacts();
    reg_alloc->ResetAllocations();
    const auto input_initial = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan initial{block,
                       reg_alloc,
                       {.xmm_resident = true},
                       {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR},
                       facts,
                       workspace,
                       input_initial};
    initial.AllocateRegisters();
    ASSERT(initial.Verify());
    reg_alloc->MapRegister(tied_value_id, HostFPR{target});
    features.ra_aes_chain_tie = true;
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {.xmm_resident = true},
                    {.gpr_reserve = 0, .fpr_reserve = 0},
                    facts,
                    workspace,
                    input_scan};
    scan.CoalesceFPRPublications(true);
}

void RegisterAllocTestSupport::RunForScalarFPRTieTest(ir::Block* block,
                                                      compile::AllocationWriter* reg_alloc,
                                                      bool enabled) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    features.sse_scalar_tie = enabled;
    reg_alloc->SetAllocationUnit(block, nullptr);
    auto& facts = reg_alloc->PrepareFacts();
    reg_alloc->ResetAllocations();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {.scalar_insert = true, .xmm_resident = true},
                    {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR},
                    facts,
                    workspace,
                    input_scan};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocTestSupport::RunForScalarFPRTieConflictTest(ir::Block* block,
                                                              compile::AllocationWriter* reg_alloc,
                                                              u32 tied_value_id,
                                                              u16 target) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    features.sse_scalar_tie = false;
    reg_alloc->SetAllocationUnit(block, nullptr);
    auto& facts = reg_alloc->PrepareFacts();
    reg_alloc->ResetAllocations();
    const auto input_initial = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan initial{block,
                       reg_alloc,
                       {.scalar_insert = true, .xmm_resident = true},
                       {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR},
                       facts,
                       workspace,
                       input_initial};
    initial.AllocateRegisters();
    ASSERT(initial.Verify());
    reg_alloc->MapRegister(tied_value_id, HostFPR{target});
    features.sse_scalar_tie = true;
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {.scalar_insert = true, .xmm_resident = true},
                    {.gpr_reserve = 0, .fpr_reserve = 0},
                    facts,
                    workspace,
                    input_scan};
    scan.CoalesceFPRPublications(true);
}

void RegisterAllocTestSupport::RunForShufpsImmTieTest(ir::Block* block,
                                                      compile::AllocationWriter* reg_alloc,
                                                      bool enabled) {
    auto& workspace = reg_alloc->Workspace();
    auto features = FeatureSet{};
    features.sse_shufps_imm = enabled;
    reg_alloc->SetAllocationUnit(block, nullptr);
    auto& facts = reg_alloc->PrepareFacts();
    reg_alloc->ResetAllocations();
    const auto input_scan = backend::arm64::DescribeAllocationInputs(
            block, *reg_alloc, features, facts, reg_alloc->Workspace());
    LinearScan scan{block,
                    reg_alloc,
                    {.xmm_resident = false},
                    {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR},
                    facts,
                    workspace,
                    input_scan};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocTestSupport::BuildSpillReloadsForTest(const ir::Block* block,
                                                        compile::AllocationWriter* allocation,
                                                        const compile::AllocationInputs& inputs) {
    auto& workspace = allocation->Workspace();
    LinearScan scan{block, allocation, {}, {}, allocation->PrepareFacts(), workspace, inputs};
    scan.BuildSpillReloads();
}

RegisterAllocTestSupport::SpillEvictTestResult RegisterAllocTestSupport::RunForSpillEvictTest(
        ir::Block* block, compile::AllocationWriter* reg_alloc, bool spill_evict) {
    auto features = FeatureSet{};
    features.ra_spill_evict = spill_evict;
    return RunForSpillEvictTest(
            block, reg_alloc, backend::arm64::PrepareAllocationInputs(block, *reg_alloc, features));
}

RegisterAllocTestSupport::SpillEvictTestResult RegisterAllocTestSupport::RunForSpillEvictTest(
        ir::Block* block, compile::AllocationWriter* reg_alloc, compile::AllocationInputs inputs) {
    SpillEvictTestResult result{};
    const auto intwidth_tie = inputs.features.ra_intwidth_tie;
    const auto spill_evict = inputs.features.ra_spill_evict;
    AllocateVerified(
            block, reg_alloc, std::move(inputs), false, false, intwidth_tie, spill_evict, &result);
    return result;
}
}  // namespace swift::runtime::ir
