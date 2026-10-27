#include <algorithm>
#include "allocation_preparation.h"
#include "fpr_contracts.h"
#include "lower_consumers.h"
#include "lower_edges.h"
#include "lower_flags.h"
#include "lower_integer_flags.h"
#include "lower_memory.h"
#include "lower_predicates.h"
#include "lower_scalar.h"
#include "lower_vector.h"
#include "residences.h"
#include "runtime/common/perf_stats.h"
#include "runtime/compile/workspace.h"

#include "runtime/backend/arm64/compile/register_contract.h"

namespace swift::runtime::backend::arm64 {

AllocationPreparation::AllocationPreparation(compile::Allocation& result,
                                             compile::SemanticFacts& facts,
                                             compile::CompileWorkspace& workspace,
                                             UnitActions& actions)
        : AllocationWriter(result, facts, workspace, kX86FixedGPRHomes), actions(actions) {
    ASSERT(!actions.Ready() && actions.allocation.RefersToSameResult(result.View()));
    const auto& features = GetFeatures();
    // Trampolines 暴露跨 module 的最大 GPR 并集。XPOOL 关闭时在 unit 私有
    // allocator 里恢复旧保留集，保持默认发码不变，同时允许 module 快照分叉。
    if (!ScratchXPoolEnabled(features)) {
        for (u32 code : {11u, 12u, 13u, 16u, 17u}) {
            DeclareGuestGPR(code);
        }
    }
    // A-class last_result home. XPOOL ON otherwise puts x12 in the value
    // pool; pin it out so later token emission can keep it live across the
    // dispatcher. XPOOL OFF already reserved it above.
    if (FlagsRegsEnabled()) {
        DeclareGuestGPR(12);
    }
    // Preserve the existing per-unit pool control without an AFP dependency.
    if (!features.fpr_ipv_reclaim) {
        for (u32 code = 11; code <= 14; ++code) {
            ReserveFPR(code);
        }
    }
}

void AllocationPreparation::BeginTargetPlanning() {
    BeginLocationEditing();
    actions.ready = false;
    actions.building = true;
    actions.generation = GPRLocationGeneration();
    actions.attempt = AllocationAttempt();
    actions.blocks.clear();
    actions.proofs.clear();
    actions.shuffle_controls.clear();
    actions.scalar_fpr.clear();
    actions.selects.clear();
    actions.flags_tokens.clear();
    actions.narrow.clear();
    actions.carry.clear();
    actions.memory.clear();
    actions.consumers.clear();
    actions.alu_split.clear();
    actions.integer_flags.clear();
    actions.integer_branches.clear();
    actions.publications.clear();
    actions.memory_transfers.clear();
    actions.addresses.clear();
    actions.pseudo_flags.clear();
    actions.flags_only_spills.clear();
    actions.unit_scratch = {};
    actions.scratch_ranges.clear();
    actions.spill_inputs.clear();
    actions.residences.clear();
    actions.certificate.reset();
    actions.shared_host_calls.reset();
    actions.shared_host_call_count = 0;
    actions.deferred_locations.reset();
    actions.deferred_location_count = 0;
    actions.edges = {};
    actions.atomics = {};
    actions.backedges.clear();
    actions.pfaf.clear();
    actions.consumer_revision = 0;
}

void AllocationPreparation::SealTargetEdits() {
    ASSERT(actions.building && actions.allocation.RefersToSameResult(*this) &&
           actions.generation == GPRLocationGeneration() && !GPRLocationsFrozen());
    CloseLocationEdits();
    // Committed target edits advance the allocation attempt within this
    // location session. Bind certificates to the final physical result.
    actions.attempt = AllocationAttempt();
}

void AllocationPreparation::EndTargetPlanning() {
    SealTargetEdits();
    actions.SealPayloads();
    if (Perf2Enabled() && actions.unit_scratch.prepared) {
        auto& stats = GetPerfStats2();
        stats.scratch_units.fetch_add(1, std::memory_order_relaxed);
        stats.scratch_frozen_ranges.fetch_add(actions.scratch_ranges.size(),
                                              std::memory_order_relaxed);
        stats.scratch_frozen_inputs.fetch_add(actions.spill_inputs.size(),
                                              std::memory_order_relaxed);
        stats.scratch_frozen_capacity_bytes.fetch_add(
                actions.scratch_ranges.capacity() * sizeof(ScratchRange) +
                        actions.spill_inputs.capacity() * sizeof(SpillInputAction),
                std::memory_order_relaxed);
    }
    EndLocationEditing();
    actions.generation = GPRLocationGeneration();
    actions.attempt = AllocationAttempt();
    actions.building = false;
    actions.ready = true;
}

void AllocationPreparation::AbortTargetPlanning() noexcept {
    if (!TargetPlanning()) return;
    actions.ready = false;
    actions.building = false;
    actions.blocks.clear();
    actions.proofs.clear();
    actions.shuffle_controls.clear();
    actions.scalar_fpr.clear();
    actions.selects.clear();
    actions.flags_tokens.clear();
    actions.narrow.clear();
    actions.carry.clear();
    actions.memory.clear();
    actions.consumers.clear();
    actions.alu_split.clear();
    actions.integer_flags.clear();
    actions.integer_branches.clear();
    actions.publications.clear();
    actions.memory_transfers.clear();
    actions.addresses.clear();
    actions.pseudo_flags.clear();
    actions.flags_only_spills.clear();
    actions.unit_scratch = {};
    actions.scratch_ranges.clear();
    actions.spill_inputs.clear();
    actions.residences.clear();
    actions.certificate.reset();
    actions.shared_host_calls.reset();
    actions.shared_host_call_count = 0;
    actions.deferred_locations.reset();
    actions.deferred_location_count = 0;
    actions.edges = {};
    actions.atomics = {};
    actions.backedges.clear();
    actions.pfaf.clear();
    actions.consumer_revision = 0;
    AbortLocationEditing();
}

void AllocationPreparation::CertifyWidthElision(u32 id, bool low32) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    actions.Certify(id, low32 ? UnitActions::Low32 : UnitActions::Width, MapCount());
    CloseLocationEdits();
}

void AllocationPreparation::CertifyHostRead(u32 id) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    actions.Certify(id, UnitActions::HostRead, MapCount());
    CloseLocationEdits();
}

void AllocationPreparation::CertifyZeroStore(u32 id) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    actions.Certify(id, UnitActions::ZeroStore, MapCount());
    CloseLocationEdits();
}

void AllocationPreparation::DeclareElidedPublication(u32 id) { ElidePublication(id); }

void AllocationPreparation::StoreSharedHostCalls(std::span<const u64> targets) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen() && !actions.shared_host_calls);
    ASSERT(targets.size() <= UINT16_MAX && std::is_sorted(targets.begin(), targets.end()));
    ASSERT(std::adjacent_find(targets.begin(), targets.end()) == targets.end());
    if (targets.empty()) return;
    ASSERT(targets.front() != 0);
    std::unique_ptr<u64[]> storage{new u64[targets.size()]};
    std::copy(targets.begin(), targets.end(), storage.get());
    actions.shared_host_calls = std::move(storage);
    actions.shared_host_call_count = static_cast<u16>(targets.size());
}

void AllocationPreparation::StoreDeferredLocations(std::span<const u16> instructions) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen() && !actions.deferred_locations);
    ASSERT(instructions.size() <= UINT16_MAX &&
           std::is_sorted(instructions.begin(), instructions.end()));
    ASSERT(std::adjacent_find(instructions.begin(), instructions.end()) == instructions.end());
    if (instructions.empty()) return;
    ASSERT(instructions.back() < MapCount());
    std::unique_ptr<u16[]> storage{new u16[instructions.size()]};
    std::copy(instructions.begin(), instructions.end(), storage.get());
    actions.deferred_locations = std::move(storage);
    actions.deferred_location_count = static_cast<u16>(instructions.size());
}

void AllocationPreparation::CertifyFPR(const FPRContract& contract) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen() && contract.valid);
    for (const auto& instruction : contract.instructions)
        actions.CertifyFPR(instruction.id, instruction.actions, instruction.control, MapCount());
}

void AllocationPreparation::StoreScalarFPR(const ScalarFPRActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& action : lowered.actions) {
        ASSERT(action.instruction < MapCount() && action.target >= 16 && action.target <= 31);
        actions.scalar_fpr.push_back(action);
    }
    for (const auto id : lowered.covered) actions.Certify(id, UnitActions::Covered, MapCount());
}

void AllocationPreparation::DeclareBlock(const ir::Block* block,
                                         const compile::BlockSummary& summary,
                                         compile::PseudoFlagsView flags) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    const auto [position, inserted] = actions.blocks.try_emplace(block);
    ASSERT(inserted && block);
    auto& row = position->second;
    row.pushes_return_address = summary.HasReturnAddressPush();
    if (flags.Empty()) return;
    ASSERT(u32{flags.first_id} + flags.count <= MapCount());
    ASSERT(actions.pseudo_flags.size() + flags.count <= MapCount());
    row.flags_offset = static_cast<u32>(actions.pseudo_flags.size());
    row.instruction_first = flags.first_id;
    row.instruction_last = static_cast<u16>(flags.first_id + flags.count - 1);
    const auto words = flags.Words();
    actions.pseudo_flags.insert(actions.pseudo_flags.end(), words.begin(), words.end());
}

void AllocationPreparation::StoreEdges(LoweredEdges lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen() && !actions.certificate);
    ASSERT(lowered.certificate.action_owner == &actions);
    actions.certificate = std::make_unique<const UnitCertificate>(std::move(lowered.certificate));
    actions.edges = std::move(lowered.actions);
}

void AllocationPreparation::StoreAtomics(AtomicActions lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    actions.atomics = std::move(lowered);
}

void AllocationPreparation::StoreBackedge(const ir::Block* block, BackedgeSelection selection) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    auto& row = actions.blocks.at(block);
    ASSERT(row.has_scratch && row.backedge == UINT16_MAX && actions.backedges.size() < UINT16_MAX);
    auto& action = selection.action;
    ASSERT(action.final_advance < MapCount());
    ASSERT(!action.omit_polarity_load || action.polarity_load < MapCount());
    ASSERT(!action.omit_polarity_store || action.polarity_store < MapCount());
    if (selection.pfaf) {
        ASSERT(actions.pfaf.size() < UINT16_MAX && selection.pfaf->producer < MapCount());
        action.pfaf = static_cast<u16>(actions.pfaf.size());
        actions.pfaf.push_back(*selection.pfaf);
    }
    row.backedge = static_cast<u16>(actions.backedges.size());
    actions.backedges.push_back(action);
}

void AllocationPreparation::StoreFlagsOnlySpills(std::span<const FlagsOnlySpillAction> rows) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& row : rows) {
        ASSERT(row.consumer < MapCount() && row.producer < MapCount());
        actions.flags_only_spills.push_back(row);
    }
}

void AllocationPreparation::StoreGPRResidences(const ir::Block* block,
                                               GPRResidences lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen() && actions.blocks.contains(block));
    ASSERT(lowered.block == block);
    for (const auto& row : lowered.rows)
        ASSERT(row.instruction < MapCount() &&
               (row.result_home == UINT8_MAX || row.result_home < 31));
    // The first block transfers its final buffer. Later blocks append their
    // already compact rows; no pointer sets or map need another projection.
    if (actions.residences.empty())
        actions.residences = std::move(lowered.rows);
    else
        actions.residences.insert(
                actions.residences.end(), lowered.rows.begin(), lowered.rows.end());
}

void AllocationPreparation::StoreScratch(const ir::Block* block, const ScratchActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen() && lowered.unit.prepared);
    auto& row = actions.blocks.at(block);
    ASSERT(!row.has_scratch);
    if (actions.unit_scratch.prepared) ASSERT(actions.unit_scratch == lowered.unit);
    actions.unit_scratch = lowered.unit;
    row.has_scratch = true;
    u16 first = UINT16_MAX, last = 0;
    for (const auto& action : lowered.ranges) {
        ASSERT(action.first <= action.last && action.last < MapCount());
        first = std::min(first, action.first);
        last = std::max(last, action.last);
        actions.scratch_ranges.push_back(action);
    }
    if (row.flags_offset != UINT32_MAX)
        ASSERT(row.instruction_first == first && row.instruction_last == last);
    row.instruction_first = first;
    row.instruction_last = last;
    for (const auto& input : lowered.inputs) {
        ASSERT(input.consumer < MapCount() && input.producer < MapCount());
        ASSERT(!input.definition ||
               (input.definition->GetBlock() == block && input.definition->Id() == input.producer));
        actions.spill_inputs.push_back(input);
    }
}

void AllocationPreparation::StoreSelects(const SelectActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& action : lowered.actions) {
        ASSERT(action.instruction < MapCount());
        actions.selects.push_back(action);
    }
    for (const auto id : lowered.covered) actions.Certify(id, UnitActions::Covered, MapCount());
}

void AllocationPreparation::StoreFlagsTokens(const FlagsTokenActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& action : lowered.retentions) {
        ASSERT(action.producer < UINT16_MAX && action.producer < MapCount() && action.homes &&
               !(action.homes & (1u << 31)));
        ASSERT(action.point == UINT16_MAX || action.point < MapCount());
        actions.flags_tokens.push_back(action);
    }
}

void AllocationPreparation::StoreScalar(const ScalarActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& action : lowered.operations) {
        ASSERT(action.instruction < MapCount());
        actions.narrow.push_back(action);
    }
    for (const auto& row : lowered.flags)
        actions.Certify(row.instruction,
                        static_cast<UnitActions::Proof>(static_cast<u16>(row.flags) << 11),
                        MapCount());
}

void AllocationPreparation::StoreCarry(const CarryActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& action : lowered.actions) {
        ASSERT(action.instruction < MapCount() && action.combine < MapCount());
        if (action.invert != UINT16_MAX) {
            ASSERT(action.invert < MapCount());
            actions.Certify(action.invert, UnitActions::CarryInvert, MapCount());
        }
        actions.carry.push_back(action);
    }
}

void AllocationPreparation::StoreMemory(const MemoryActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& action : lowered.actions) {
        ASSERT(action.instruction < MapCount() && action.bits);
        ASSERT(!action.Has(MemoryAction::Direct) ||
               (action.Has(MemoryAction::Narrow) && action.target < 31));
        ASSERT(!action.Has(MemoryAction::PostIndex) || action.base < 31);
        ASSERT(!(action.Has(MemoryAction::PostIndex) && action.Has(MemoryAction::Offset)));
        actions.memory.push_back(action);
    }
    for (const auto id : lowered.covered) actions.Certify(id, UnitActions::Covered, MapCount());
}

void AllocationPreparation::StoreConsumers(const ConsumerActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen() && lowered.valid);
    for (const auto& row : lowered.actions) {
        ASSERT(row.instruction < MapCount());
        ASSERT(row.related == UINT16_MAX || row.related < MapCount());
        ASSERT(!row.Has(ConsumerFlag::IncomingZero) || row.auxiliary <= 1);
        actions.consumers.push_back(row);
    }
    for (const auto& row : lowered.audit) {
        ASSERT(row.instruction < MapCount());
        actions.alu_split.push_back(row);
    }
    ++actions.consumer_revision;
    ASSERT(actions.consumer_revision);
}

void AllocationPreparation::StoreIntegerFlags(const IntegerFlagsActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& row : lowered.actions) {
        ASSERT(row.instruction < MapCount());
        ASSERT(!row.IsInput() || row.argument < MapCount());
        ASSERT(!row.IsCompare() || row.Width() == 1 || row.Width() == 2);
        actions.integer_flags.push_back(row);
    }
    for (const auto id : lowered.covered) actions.Certify(id, UnitActions::Covered, MapCount());
}

void AllocationPreparation::StoreIntegerBranch(const IntegerBranchActions& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    const auto& row = lowered.action;
    ASSERT(row.producer < row.condition && row.condition < MapCount());
    ASSERT(!row.ZeroTarget() || (row.zero_target < 31 && (row.width == 4 || row.width == 8)));
    ASSERT(row.ZeroTarget() || row.width == 0 || row.IsNarrow());
    actions.integer_branches.push_back(row);
    for (const auto id : lowered.covered) actions.Certify(id, UnitActions::Covered, MapCount());
}

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {
void AllocationPreparation::StorePublications(std::span<const PublicationAction> lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& row : lowered) {
        ASSERT(row.instruction < MapCount() && row.target < 31);
        ASSERT(!row.SourceHome() || *row.SourceHome() < 31);
        ASSERT(row.kind != PublicationAction::BiasedUpdate ||
               (row.parameter > 0 && row.parameter <= 256));
        actions.publications.push_back(row);
    }
}
void AllocationPreparation::StoreMemorySelections(const MemorySelections& lowered) {
    ASSERT(TargetPlanning() && !GPRLocationsFrozen());
    for (const auto& row : lowered.Transfers()) {
        ASSERT(row.instruction < MapCount() && row.related < MapCount() && row.home < 31);
        actions.memory_transfers.push_back(row);
    }
    for (const auto& row : lowered.Addresses()) {
        ASSERT(row.instruction < MapCount() && row.memory < MapCount() && row.home < 31);
        actions.addresses.push_back(row);
    }
}
}  // namespace swift::runtime::backend::arm64
