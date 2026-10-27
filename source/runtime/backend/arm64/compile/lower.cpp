#include <algorithm>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include "aarch64/assembler-aarch64.h"
#include "base/scope_exit.h"
#include "call_preparation.h"
#include "internal.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/certificates.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/backend/arm64/compile/fpr_preparation.h"
#include "runtime/backend/arm64/compile/lower_atomics.h"
#include "runtime/backend/arm64/compile/lower_consumers.h"
#include "runtime/backend/arm64/compile/lower_edges.h"
#include "runtime/backend/arm64/compile/lower_flags.h"
#include "runtime/backend/arm64/compile/lower_integer_flags.h"
#include "runtime/backend/arm64/compile/lower_predicates.h"
#include "runtime/backend/arm64/compile/lower_scalar.h"
#include "runtime/backend/arm64/compile/memory_preparation.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/scratch_preparation.h"
#include "runtime/backend/arm64/compile/vector_preparation.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/gpr_coalescing_contract.h"
#include "runtime/common/perf_stats.h"
#include "runtime/common/svm_config.h"
#include "runtime/common/variant_util.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {
TargetPreparation::PendingGPRBlock TargetPreparation::DeclareGPRBlock(const ir::Block* block) {
    PerfScope2 perf_declare{GetPerfStats2().plan_declare};
    cur_block = block;
    publications.clear();
    {
        PerfScope2 perf_guestmap{GetPerfStats2().plan_guestmap};
        register_state.BeginBlock(block, features, allocation.PrepareFacts(block));
    }
    {
        PerfScope2 perf_builder{GetPerfStats2().plan_builder};
        gpr_builder.Prepare(block);
    }
    memory_selections.Reset(gpr_builder.UseFacts());
    PerfScope2 perf_declare_pinned{GetPerfStats2().declare_pinned};
    const auto& skip = GetSvmConfig().skip_prep;
    if (skip.find("loadupd") == std::string::npos) PreparePinnedLoadUpdates(block);
    if (skip.find("memvals") == std::string::npos) PreparePinnedMemoryValues(block);
    if (skip.find("spillpub") == std::string::npos)
        gpr_builder.PrepareSpilledGPRPublications();
    gpr_builder.PrepareNarrowReads(
            [&](const ir::Inst* consumer) { return GetPseudoFlags(consumer).set; });
    PreparePinnedMemoryAddresses(block);
    perf_declare_pinned.Stop();
    PerfScope2 perf_declare_tail{GetPerfStats2().declare_tail};
    {
        PerfScope2 perf_zext{GetPerfStats2().tail_zext};
        PreparePinnedZeroExtendPublications(block);
    }
    PrepareMemoryUpdates(block);
    PreparePublications(block);
    auto* analysis = &gpr_builder.PrepareUseFacts();
    // Preserve the old pointer-keyed publication order for physical admission
    // and PF candidate seeding. Frozen rows are sorted by ID at final sealing.
    std::sort(publications.begin(), publications.end(), [&](const auto& left, const auto& right) {
        return std::less<const ir::Inst*>{}(analysis->FindInstruction(left.instruction),
                                            analysis->FindInstruction(right.instruction));
    });
    for (const auto& action : publications)
        if (action.kind == PublicationAction::Skip)
            allocation.DeclareElidedPublication(action.instruction);
    PerfScope2 perf_finish{GetPerfStats2().tail_finish};
    auto draft = gpr_builder.FinishConstraints();
    perf_finish.Stop();
    perf_declare_tail.Stop();
    return {block, std::move(draft), analysis, memory_selections.Finish(), std::move(publications)};
}

AllocationPreparationResult TargetPreparation::Complete(const ir::Block* block,
                                                        const ir::HIRFunction* function) {
    const bool ssa_valid = allocation.PrepareFacts().PrepareSSAValidation();
    if (!ssa_valid) {
        ASSERT_MSG(!GetSvmConfig().verify_plan, "invalid SSA input to ARM64 preparation");
        // Record a rejected certificate without running target analyses over
        // an unowned CFG reference or an unavailable definition.
        allocation.BeginTargetPlanning();
        allocation.SealTargetEdits();
        allocation.StoreEdges(
                {CaptureUnitCertificate(
                         block, function, allocation, allocation.Actions(), config, features),
                 {}});
        allocation.EndTargetPlanning();
        return AllocationPreparationResult::Ready;
    }
    std::vector<const ir::Block*> blocks;
    blocks.reserve(function ? function->GetHIRBlocksRPO().size() : 1);
    if (function) {
        for (auto& item : function->GetHIRBlocksRPO()) blocks.push_back(item.GetBlock());
    } else
        blocks.push_back(block);
    std::vector<FPRContract> fpr_contracts;
    {
        PerfScope2 perf_targets{GetPerfStats2().plan_targets};
        for (size_t i = 0; i < blocks.size(); ++i) {
            auto* current = blocks[i];
            auto& analysis = allocation.PrepareFacts(current).PrepareIndex(current);
            if (analysis.HasVectorConstants()) SelectPshufdDirect(analysis, allocation);
            if (!analysis.HasFPRInstructions()) continue;
            auto fpr = PrepareFPRContract(current,
                                          allocation,
                                          features,
                                          config.sse_scalar_insert,
                                          features.sse_scalar_tie,
                                          analysis,
                                          workspace);
            // Reject before GPR edits or action construction. The allocation
            // controller owns the retry and retains the same semantic facts.
            if (!fpr.valid) return AllocationPreparationResult::RetryWithoutFPRCoalescing;
            if (!fpr.instructions.empty()) {
                if (fpr_contracts.empty()) fpr_contracts.resize(blocks.size());
                fpr_contracts[i] = std::move(fpr);
            }
        }
    }
    auto rollback = ScopeExit([&] { allocation.AbortTargetPlanning(); });
    allocation.BeginTargetPlanning();
    if (function) register_state.BeginFunction(*function, features, allocation.PrepareFacts());
    std::vector<PendingGPRBlock> pending;
    pending.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        pending.push_back(DeclareGPRBlock(blocks[i]));
        if (!fpr_contracts.empty()) {
            const auto contract = std::move(fpr_contracts[i]);
            allocation.CertifyFPR(contract);
        }
    }
    // CertifyFPR copies compact proofs. No declaration input is borrowed by
    // the following lowering phase or the final emission result.
    std::vector<FPRContract>{}.swap(fpr_contracts);
    std::vector<u32> zero_stores;
    const auto unit_scratch = PrepareUnitScratch(allocation.GetGprs(), features);
    BitVector flag_coverage(allocation.MapCount());
    PerfScope2 perf_perblock{GetPerfStats2().plan_perblock};
    for (auto& item : pending) {
        // Publication and memory inputs die with this block. Physical writes
        // remain pending for final certification.
        auto block_publications = std::move(item.publications);
        auto block_memory = std::move(item.memory);
        allocation.StorePublications(block_publications);
        allocation.StoreMemorySelections(block_memory);
        if (FlagsRegsEnabled()) {
            std::vector<const ir::Inst*> publications;
            publications.reserve(block_publications.size());
            for (const auto& action : block_publications)
                publications.push_back(item.analysis->FindInstruction(action.instruction));
            allocation.StoreFlagsTokens(LowerFlagsTokens(*item.analysis,
                                                         workspace,
                                                         allocation,
                                                         features,
                                                         publications,
                                                         item.analysis->PseudoFlags()));
        }
        std::optional<ConsumerActions> consumers;
        SelectActions selects;
        if (item.analysis->HasBooleanSelects() &&
            GetSvmConfig().skip_prep.find("boolsel") == std::string::npos) {
            selects = LowerSelects(*item.analysis, workspace, [&](const ir::Inst& inst) {
                // Pure IR needs only the target encoding contract. Reuse the
                // helper/effect contracts for operations with observable state.
                return inst.GetEffects() == ir::IREffect::None
                               ? TargetPreservesHostNZCV(inst.GetOp())
                               : TargetRetainsPendingHostNZCV(inst, features) &&
                                         !TargetMayFaultOrObserve(inst, features);
            });
            allocation.StoreSelects(selects);
        }
        const bool alu_split_audit =
                GetSvmConfig().density_prof && GetSvmConfig().ra_hot_coalesce_all;
        if (item.analysis->HasConsumerDecisions() || alu_split_audit) {
            const auto& terminal = item.block->GetTerminal();
            consumers.emplace(LowerConsumers(
                    *item.analysis,
                    workspace,
                    [&](const ir::Inst& inst) {
                        return TargetRetainsPendingHostNZCV(inst, features);
                    },
                    [&](const ir::Inst& inst) {
                        return TargetMayFaultOrObserve(inst, features);
                    },
                    &terminal,
                    alu_split_audit));
            ASSERT_MSG(consumers->valid, "invalid pair-result relationship before emission");
        }
        auto vector = PrepareVector(item.block, allocation, *item.analysis, workspace);
        const auto& scalar = vector.publications;
        allocation.StoreScalarFPR(scalar);
        auto memory =
                item.analysis->HasMemoryInstructions()
                        ? PrepareMemory(item.block,
                                        allocation,
                                        features,
                                        block_memory,
                                        item.draft.residences,
                                        {.narrow_fuse = features.mem_narrow_fuse,
                                         .address_tie = features.addr_ea_tie,
                                         .biased = use_memory_base,
                                         .rcpc = True(config.arm64_features & Arm64Features::RCpc)},
                                        *item.analysis)
                        : MemoryActions{};
        allocation.StoreMemory(memory);
        for (const auto& write : memory.writes)
            item.draft.writes.push_back(
                    {write.instruction, write.base, write.consumed_value, write.update});
        std::optional<IntegerBranchActions> branch;
        if (item.block->HasDeadEdgeIntegerBranchProof()) {
            branch = LowerIntegerBranch(item.block,
                                        *item.analysis,
                                        workspace,
                                        allocation,
                                        features,
                                        allocation.PrepareFixedGPRHomes());
            if (branch) allocation.StoreIntegerBranch(*branch);
        }
        for (const auto id : memory.covered) flag_coverage.set(id);
        for (const auto id : scalar.covered) flag_coverage.set(id);
        if (branch)
            for (const auto id : branch->covered) flag_coverage.set(id);
        auto integer_flags =
                GetSvmConfig().skip_prep.find("intflags") != std::string::npos
                        ? IntegerFlagsActions{}
                        : LowerIntegerFlags(
                                  *item.analysis,
                                  workspace,
                                  allocation,
                                  item.analysis->PseudoFlags(),
                                  branch ? &branch->action : nullptr,
                                  flag_coverage,
                                  consumers && consumers->pstate ? &*consumers->pstate : nullptr,
                                  GetSvmConfig().flags_cfinv &&
                                          True(config.arm64_features & Arm64Features::FlagM));
        allocation.StoreIntegerFlags(integer_flags);
        const auto& resident = vector.resident;
        allocation.StoreScalarFPR(resident);
        const auto copies = LowerScalarCopies(*item.analysis);
        allocation.StoreScalar(copies);
        // A raw carry action suppresses CFINV and later consumes that exact
        // pending host-C representation. Other configurations use normal flags.
        if (consumers && FlagsRegsEnabled() && GetSvmConfig().flags_cfinv &&
            True(config.arm64_features & Arm64Features::FlagM))
            allocation.StoreCarry(
                    LowerCarryConditions(item.block, *item.analysis, features, *consumers));
        // Match preparation-time coverage; later Backedge/PFAF stays excluded.
        for (const auto id : integer_flags.covered) flag_coverage.set(id);
        for (const auto id : selects.covered) flag_coverage.set(id);
        for (const auto id : resident.covered) flag_coverage.set(id);
        allocation.StoreScalar(LowerNarrowBits(*item.analysis,
                                               workspace,
                                               allocation,
                                               {&copies,
                                                &integer_flags,
                                                branch ? &branch->action : nullptr,
                                                &flag_coverage}));
        if (features.zero_store_zr) {
            std::vector<const ir::Inst*> incoming;
            const auto local = ProveZeroStores(*item.analysis, allocation, &incoming);
            zero_stores.insert(zero_stores.end(), local.begin(), local.end());
            if (!incoming.empty()) {
                if (!consumers) consumers.emplace();
                consumers->AddIncomingZeros(*item.analysis, incoming);
            }
        }
        if (consumers) allocation.StoreConsumers(*consumers);
        allocation.DeclareBlock(
                item.block, item.analysis->Summary(), item.analysis->PseudoFlags().View());
        {
            PerfScope2 perf_seal{GetPerfStats2().perblock_seal};
            allocation.StoreGPRResidences(item.block, std::move(item.draft.residences));
            allocation.StoreScratch(item.block,
                                    PrepareScratch(item.block,
                                                   allocation,
                                                   features,
                                                   unit_scratch,
                                                   *item.analysis,
                                                   workspace));
        }
    }
    perf_perblock.Stop();
    if (function) {
        PerfScope2 perf_calls{GetPerfStats2().plan_targets};
        allocation.StoreSharedHostCalls(
                PrepareSharedHostCalls(*function, allocation.PrepareFacts()));
    }
    Finalize(block, function, blocks, pending, zero_stores, ssa_valid);
    rollback.Cancel();
    return AllocationPreparationResult::Ready;
}

}  // namespace swift::runtime::backend::arm64
