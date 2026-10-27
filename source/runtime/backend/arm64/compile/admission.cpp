#include "actions.h"
#include "admission.h"

#include <algorithm>
#include <type_traits>
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/analysis/cfg.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {
namespace {
const ir::HIRBlock* Identity(const auto& item) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(item)>>)
        return item;
    else
        return &item;
}
std::vector<const ir::HIRBlock*> CapturePointers(const auto& range) {
    std::vector<const ir::HIRBlock*> result;
    if constexpr (requires { range.size(); }) result.reserve(range.size());
    for (auto& item : range) result.push_back(Identity(item));
    return result;
}
bool MatchesPointers(const auto& range, std::span<const ir::HIRBlock* const> frozen) {
    size_t index = 0;
    for (const auto& item : range) {
        if (index >= frozen.size() || Identity(item) != frozen[index++]) return false;
    }
    return index == frozen.size();
}
bool MatchesProvenance(const std::vector<ir::FunctionEntryProvenance>& current,
                       const std::vector<ir::FunctionEntryProvenance>& frozen) {
    if (current.size() != frozen.size()) return false;
    for (size_t i = 0; i < current.size(); ++i) {
        const auto& a = current[i];
        const auto& b = frozen[i];
        if (a.target != b.target || a.owner_start != b.owner_start || a.owner_end != b.owner_end ||
            a.dependencies != b.dependencies || a.disposition != b.disposition ||
            a.rejection != b.rejection || a.call_return_owned != b.call_return_owned ||
            a.call_return_boundary != b.call_return_boundary || a.external_root != b.external_root)
            return false;
    }
    return true;
}
}  // namespace

EdgeOptions CaptureEdgeOptions(const Config& config, const FeatureSet& features) {
    const auto& svm = GetSvmConfig();
    return {config.region_edges,
            FlagsRegsEnabled(),
            features.helper_leaf_abi,
            BackedgeLatchEnabled(),
            svm.backedge_flags,
            features.flags_loop_lazy,
            features.flags_region_branch,
            svm.flags_cfinv,
            True(config.arm64_features & Arm64Features::FlagM),
            svm.density_prof && svm.ra_hot_coalesce_all,
            True(config.arm64_features & Arm64Features::Atomics),
            config.memory_base != nullptr || config.page_table != nullptr,
            features.flags_regs_audit};
}

UnitCertificate CaptureUnitCertificate(const ir::Block* block,
                                       const ir::HIRFunction* function,
                                       compile::AllocationView allocation,
                                       const UnitActions& unit_actions,
                                       const Config& config,
                                       const FeatureSet& features) {
    UnitCertificate plan;
    plan.action_owner = &unit_actions;
    plan.consumer_revision = unit_actions.ConsumerRevision();
    plan.options = CaptureEdgeOptions(config, features);
    plan.scratch_inputs = CaptureScratchPreparationInputs(allocation.GetGprs(), features);
    plan.standalone = function ? nullptr : block;
    plan.function = function;
    plan.generation = allocation.GPRLocationGeneration();
    plan.allocation_attempt = allocation.AllocationAttempt();
    const auto capture = [&](const ir::HIRBlock* hir, const ir::Block* current) {
        BlockCertificate certificate{.hir = hir,
                                     .block = current,
                                     .location = current->GetStartLocation().Value(),
                                     .end_location = current->GetEndLocation().Value()};
        if (hir) {
            certificate.predecessor_begin = plan.predecessors.size();
            for (auto* predecessor : hir->GetPredecessors())
                plan.predecessors.push_back(predecessor);
            certificate.predecessor_count =
                    plan.predecessors.size() - certificate.predecessor_begin;
            certificate.successor_begin = plan.successors.size();
            for (auto* successor : hir->GetSuccessors()) plan.successors.push_back(successor);
            certificate.successor_count = plan.successors.size() - certificate.successor_begin;
            certificate.call_return = hir->GetCallReturnBlock();
            certificate.call_return_target = hir->IsCallReturnBlock();
        }
        if (unit_actions.HasPreparedScratch(current))
            certificate.emission = unit_actions.ForBlock(current);
        plan.blocks.push_back(std::move(certificate));
    };
    if (!function) {
        capture(nullptr, block);
        return plan;
    }
    plan.root = function->GetFunction();
    plan.root_location = plan.root->GetStartLocation().Value();
    plan.root_end = plan.root->GetEndLocation().Value();
    plan.valid = compile::FunctionCFGClosed(*function);
    if (!plan.valid) return plan;
    plan.all_members = CapturePointers(function->GetHIRBlocks());
    plan.listed_members = CapturePointers(function->GetHIRBlockList());
    plan.rpo = CapturePointers(function->GetHIRBlocksRPO());
    plan.external_roots = CapturePointers(function->GetExternalEntryRoots());
    for (const auto& edge : function->GetExternalDirectLinks())
        plan.external_links.emplace_back(edge.source, edge.target.Value());
    plan.provenance = function->GetFunctionEntryProvenance();
    // HIR block order IDs index the already captured complete member roster.
    // Reject foreign CFG references rather than certifying only their pointer
    // and later reading unowned IR through canonical-entry analysis.
    plan.blocks.reserve(plan.all_members.size());
    plan.predecessors.reserve(plan.all_members.size() * 2);
    plan.successors.reserve(plan.all_members.size() * 2);
    for (auto* hir : plan.all_members)
        if (hir) capture(hir, hir->GetBlock());
    return plan;
}

bool UnitCertificate::MatchesUnit(const ir::Block* current_block,
                                  const ir::HIRFunction* current_function,
                                  compile::AllocationView allocation,
                                  const UnitActions& unit_actions,
                                  const Config& config,
                                  const FeatureSet& features) const {
    if (!valid || !ssa_valid || !entry_state_valid || action_owner != &unit_actions ||
        consumer_revision != unit_actions.ConsumerRevision() ||
        !unit_actions.MatchesAllocation(allocation) || function != current_function ||
        standalone != current_block || generation != allocation.GPRLocationGeneration() ||
        allocation_attempt != allocation.AllocationAttempt() ||
        !MatchesTargetInputs(allocation, config, features))
        return false;
    if (function) {
        if (root != function->GetFunction() || root_location != root->GetStartLocation().Value() ||
            root_end != root->GetEndLocation().Value() ||
            !MatchesPointers(function->GetHIRBlocks(), all_members) ||
            !MatchesPointers(function->GetHIRBlockList(), listed_members) ||
            !MatchesPointers(function->GetHIRBlocksRPO(), rpo) ||
            !MatchesPointers(function->GetExternalEntryRoots(), external_roots) ||
            !MatchesProvenance(function->GetFunctionEntryProvenance(), provenance))
            return false;
        const auto& links = function->GetExternalDirectLinks();
        if (links.size() != external_links.size()) return false;
        for (size_t i = 0; i < links.size(); ++i)
            if (links[i].source != external_links[i].first ||
                links[i].target.Value() != external_links[i].second)
                return false;
    }
    for (const auto& proof : blocks) {
        if ((proof.hir &&
             (proof.hir->GetBlock() != proof.block ||
              !MatchesPointers(proof.hir->GetPredecessors(),
                               std::span<const ir::HIRBlock* const>{predecessors}.subspan(
                                       proof.predecessor_begin, proof.predecessor_count)) ||
              !MatchesPointers(proof.hir->GetSuccessors(),
                               std::span<const ir::HIRBlock* const>{successors}.subspan(
                                       proof.successor_begin, proof.successor_count)) ||
              proof.hir->GetCallReturnBlock() != proof.call_return ||
              proof.hir->IsCallReturnBlock() != proof.call_return_target)) ||
            proof.block->GetStartLocation().Value() != proof.location ||
            proof.block->GetEndLocation().Value() != proof.end_location)
            return false;
        if (proof.emission) {
            const auto* actions = unit_actions.ForBlock(proof.block);
            if (actions != proof.emission || !unit_actions.HasPreparedScratch(proof.block))
                return false;
        }
    }
    return true;
}

bool UnitCertificate::MatchesTargetInputs(compile::AllocationView allocation,
                                          const Config& config,
                                          const FeatureSet& features) const {
    return MatchesScratchInputs(allocation.GetGprs(), features) &&
           options == CaptureEdgeOptions(config, features);
}

bool UnitCertificate::MatchesScratchInputs(const GPRSMask& pool, const FeatureSet& features) const {
    return scratch_inputs == CaptureScratchPreparationInputs(pool, features);
}

}  // namespace swift::runtime::backend::arm64
