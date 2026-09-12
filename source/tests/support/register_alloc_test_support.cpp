#include "register_alloc_test_support.h"
#include "runtime/ir/opts/linear_scan_allocator.h"
namespace swift::runtime::ir {
#include "runtime/ir/opts/register_alloc_verified.inc"
// Test entry points share the allocator's internal implementation without
// adding policy branches to the production entry points.

void RegisterAllocTestSupport::RunForIntWidthTieTest(ir::Block* block,
                                              backend::RegAlloc* reg_alloc,
                                              bool intwidth_tie) {
    auto features = FeatureSet{};
    features.ra_intwidth_tie = intwidth_tie;
    RunVerified(block, reg_alloc, features, false, false, intwidth_tie,
                features.induct_tie, features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForInductTieTest(ir::Block* block,
                                            backend::RegAlloc* reg_alloc,
                                            bool induct_tie) {
    auto features = FeatureSet{};
    features.induct_tie = induct_tie;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, induct_tie, features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForCoalesceTest(ir::Block* block,
                                           backend::RegAlloc* reg_alloc,
                                           bool coalesce) {
    auto features = FeatureSet{};
    features.ra_coalesce = coalesce;
    features.ra_coalesce_live = false;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForCoalesceLiveTest(ir::Block* block,
                                               backend::RegAlloc* reg_alloc,
                                               bool live) {
    auto features = FeatureSet{};
    features.ra_coalesce = true;
    features.ra_coalesce_live = live;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForCoalesceConflictTest(ir::Block* block,
                                                   backend::RegAlloc* reg_alloc,
                                                   u32 tied_value_id,
                                                   u16 target) {
    auto features = FeatureSet{};
    // The conflict binding below must be installed before the coalescer's
    // only run; keep the initial verified pass from coalescing on its own
    // regardless of the FeatureSet default.
    features.ra_coalesce = false;
    features.ra_coalesce_live = false;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
    // Model the output of any earlier fixed-home tie independently of its
    // current opcode-specific matcher. The coalescer must treat that mapping
    // as authoritative and reject an intersecting publication window.
    reg_alloc->MapRegister(tied_value_id, HostGPR{target});
    features.ra_coalesce = true;
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features},
        {.gpr_reserve = 0, .fpr_reserve = 0}};
    scan.CoalesceGuestRegisterAccesses();
}

void RegisterAllocTestSupport::RunForWidthChainTest(ir::Block* block,
                                             backend::RegAlloc* reg_alloc,
                                             bool enabled) {
    auto features = FeatureSet{};
    features.ra_width_chain = enabled;
    features.ra_width_chain_long = false;
    features.ra_coalesce = false;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocTestSupport::RunForWidthChainConflictTest(ir::Block* block,
                                                     backend::RegAlloc* reg_alloc,
                                                     u32 tied_value_id,
                                                     u16 target) {
    auto features = FeatureSet{};
    features.ra_width_chain = false;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
    reg_alloc->MapRegister(tied_value_id, HostGPR{target});
    features.ra_width_chain = true;
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features},
        {.gpr_reserve = 0, .fpr_reserve = 0}};
    scan.CoalesceWidthChains();
}

void RegisterAllocTestSupport::RunForWidthChainLongConflictTest(ir::Block* block,
                                                         backend::RegAlloc* reg_alloc,
                                                         u32 tied_value_id,
                                                         u16 target) {
    auto features = FeatureSet{};
    features.ra_width_chain = false;
    features.ra_width_chain_long = false;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
    reg_alloc->MapRegister(tied_value_id, HostGPR{target});
    features.ra_width_chain_long = true;
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features},
        {.gpr_reserve = 0, .fpr_reserve = 0}};
    scan.CoalesceWidthChains();
}

void RegisterAllocTestSupport::RunForXmmResidentTest(ir::Block* block,
                                              backend::RegAlloc* reg_alloc,
                                              bool enabled) {
    auto features = FeatureSet{};
    reg_alloc->ResetAllocations();
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features, .xmm_resident = enabled},
        {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR}};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocTestSupport::RunForXmmResidentConflictTest(ir::Block* block,
                                                      backend::RegAlloc* reg_alloc,
                                                      u32 tied_value_id,
                                                      u16 target) {
    auto features = FeatureSet{};
    reg_alloc->ResetAllocations();
    LinearScanAllocator initial{block, reg_alloc,
        {.features = features, .xmm_resident = false},
        {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR}};
    initial.AllocateRegisters();
    ASSERT(initial.Verify());
    reg_alloc->MapRegister(tied_value_id, HostFPR{target});
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features, .xmm_resident = false},
        {.gpr_reserve = 0, .fpr_reserve = 0}};
    scan.CoalesceGuestFPRAccesses(true);
}

void RegisterAllocTestSupport::RunForAesChainTieTest(ir::Block* block,
                                              backend::RegAlloc* reg_alloc,
                                              bool enabled) {
    auto features = FeatureSet{};
    features.ra_aes_chain_tie = enabled;
    reg_alloc->ResetAllocations();
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features, .xmm_resident = true},
        {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR}};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocTestSupport::RunForAesChainTieConflictTest(
        ir::Block* block,
        backend::RegAlloc* reg_alloc,
        u32 tied_value_id,
        u16 target) {
    auto features = FeatureSet{};
    // 初始分配必须显式关闭 tie:本用例的语义是"先无 tie 分配、再强压冲突、
    // 最后开 tie 合并验证整组件拒绝";跟随编译期默认会在第一相就打上
    // tie 标记,而第二相的拒绝无法回滚既有标记,翻盘后测试恒假。
    features.ra_aes_chain_tie = false;
    reg_alloc->ResetAllocations();
    LinearScanAllocator initial{block, reg_alloc,
        {.features = features, .xmm_resident = true},
        {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR}};
    initial.AllocateRegisters();
    ASSERT(initial.Verify());
    reg_alloc->MapRegister(tied_value_id, HostFPR{target});
    features.ra_aes_chain_tie = true;
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features, .xmm_resident = true},
        {.gpr_reserve = 0, .fpr_reserve = 0}};
    scan.CoalesceGuestFPRAccesses(true);
}

void RegisterAllocTestSupport::RunForScalarFPRTieTest(ir::Block* block,
                                               backend::RegAlloc* reg_alloc,
                                               bool enabled) {
    auto features = FeatureSet{};
    features.sse_scalar_tie = enabled;
    reg_alloc->ResetAllocations();
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features, .scalar_insert = true, .xmm_resident = true},
        {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR}};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocTestSupport::RunForScalarFPRTieConflictTest(
        ir::Block* block,
        backend::RegAlloc* reg_alloc,
        u32 tied_value_id,
        u16 target) {
    auto features = FeatureSet{};
    features.sse_scalar_tie = false;
    reg_alloc->ResetAllocations();
    LinearScanAllocator initial{block, reg_alloc,
        {.features = features, .scalar_insert = true, .xmm_resident = true},
        {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR}};
    initial.AllocateRegisters();
    ASSERT(initial.Verify());
    reg_alloc->MapRegister(tied_value_id, HostFPR{target});
    features.sse_scalar_tie = true;
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features, .scalar_insert = true, .xmm_resident = true},
        {.gpr_reserve = 0, .fpr_reserve = 0}};
    scan.CoalesceGuestFPRAccesses(true);
}

void RegisterAllocTestSupport::RunForShufpsImmTieTest(ir::Block* block,
                                               backend::RegAlloc* reg_alloc,
                                               bool enabled) {
    auto features = FeatureSet{};
    features.sse_shufps_imm = enabled;
    reg_alloc->ResetAllocations();
    LinearScanAllocator scan{block, reg_alloc,
        {.features = features, .xmm_resident = false},
        {.gpr_reserve = 0, .fpr_reserve = backend::kDefaultScratchFPR}};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

RegisterAllocTestSupport::SpillEvictTestResult RegisterAllocTestSupport::RunForSpillEvictTest(
        ir::Block* block,
        backend::RegAlloc* reg_alloc,
        bool spill_evict) {
    SpillEvictTestResult result{};
    auto features = FeatureSet{};
    features.ra_spill_evict = spill_evict;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                spill_evict, &result);
    return result;
}

}
