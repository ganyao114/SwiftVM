#include "region_preparation_plan.h"

#include <algorithm>
#include <array>
#include <type_traits>

#include "edge_flags_analysis.h"
#include "gpr_block_plan.h"
#include "target_analysis_traits.h"
#include "runtime/backend/function_entry_contract.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/common/variant_util.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {
namespace {
const ir::HIRBlock* Identity(const auto& item) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(item)>>) return item;
    else return &item;
}
std::vector<ir::HIRBlock*> CapturePointers(auto& range) {
    std::vector<ir::HIRBlock*> result;
    if constexpr (requires { range.size(); }) result.reserve(range.size());
    for (auto& item : range) result.push_back(const_cast<ir::HIRBlock*>(Identity(item)));
    return result;
}
bool MatchesPointers(const auto& range, std::span<ir::HIRBlock* const> frozen) {
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
        const auto& a = current[i]; const auto& b = frozen[i];
        if (a.target != b.target || a.owner_start != b.owner_start || a.owner_end != b.owner_end ||
            a.dependencies != b.dependencies || a.disposition != b.disposition || a.rejection != b.rejection ||
            a.call_return_owned != b.call_return_owned || a.call_return_boundary != b.call_return_boundary ||
            a.external_root != b.external_root) return false;
    }
    return true;
}
void PrepareBackwardTerminalCandidates(RegionPreparationPlan& plan, ir::Block* block) {
    std::vector<u64> targets;
    CollectRegionTargets(block->GetTerminal(), targets);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    const u64 source = block->GetStartLocation().Value();
    for (u64 target : targets) if (target < source)
        plan.backward_terminal_candidates.emplace_back(source, target);
}
} // namespace

RegionPreparationOptions CaptureRegionPreparationOptions(const Config& config,
                                                         const FeatureSet& features) {
    const auto& svm = GetSvmConfig();
    return {config.region_edges, FlagsRegsEnabled(), features.helper_leaf_abi,
            BackedgeLatchEnabled(), svm.backedge_flags, features.flags_loop_lazy,
            features.flags_region_branch, svm.flags_cfinv,
            True(config.arm64_features & Arm64Features::FlagM),
            svm.density_prof && svm.ra_hot_coalesce_all,
            True(config.arm64_features & Arm64Features::Atomics),
            config.memory_base != nullptr || config.page_table != nullptr};
}

UnalignedAtomicFallbackPlan BuildUnalignedAtomicFallbackPlan(std::span<ir::Block* const> blocks,
        const RegAlloc& allocation, bool lse, bool biased) {
    UnalignedAtomicFallbackPlan plan;
    if (!lse) return plan;
    const auto mapped = [&](ir::Value value) -> std::optional<u8> {
        if (allocation.ValueType(value) != RegAlloc::GPR) return std::nullopt;
        return static_cast<u8>(allocation.ValueGPR(value).id);
    };
    for (auto* block : blocks) for (auto& inst : block->GetInstList()) {
        UnalignedAtomicFallbackKey key{};
        ir::Value address{}, first{}, second{};
        switch (inst.GetOp()) {
            case ir::OpCode::CompareAndSwap:
                key.kind = UnalignedAtomicFallbackKind::CompareAndSwap;
                address = inst.GetArg<ir::Value>(0);
                first = inst.GetArg<ir::Value>(1);
                second = inst.GetArg<ir::Value>(2);
                break;
            case ir::OpCode::AtomicExchange:
                key.kind = UnalignedAtomicFallbackKind::Exchange;
                address = inst.GetArg<ir::Value>(0);
                first = inst.GetArg<ir::Value>(1);
                break;
            case ir::OpCode::AtomicFetchAdd:
                key.kind = UnalignedAtomicFallbackKind::FetchAdd;
                address = inst.GetArg<ir::Value>(0);
                first = inst.GetArg<ir::Value>(1);
                break;
            default: continue;
        }
        key.type = first.Type();
        const u32 width = ir::GetValueSizeByte(key.type);
        if (width != sizeof(u32) && width != sizeof(u64)) continue;
        const auto result = mapped(ir::Value{&inst}), first_code = mapped(first);
        const auto address_code = biased ? std::optional<u8>{static_cast<u8>(vixl::aarch64::mem_scratch.GetCode())}
                                         : mapped(address);
        if (!result || !first_code || !address_code || *result == *first_code) continue;
        key.address = *address_code; key.result = *result; key.first = *first_code;
        if (key.kind == UnalignedAtomicFallbackKind::CompareAndSwap) {
            const auto second_code = mapped(second);
            if (!second_code || *result == *second_code) continue;
            key.second = *second_code;
        }
        plan.keys.emplace(&inst, key);
        ++plan.counts[key];
    }
    return plan;
}

void CollectRegionTargets(const ir::Terminal& terminal, std::vector<u64>& targets) {
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) targets.push_back(term.next.Value());
        else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                           std::is_same_v<T, ir::terminal::Condition>) {
            CollectRegionTargets(term.then_, targets); CollectRegionTargets(term.else_, targets);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) CollectRegionTargets(item.then, targets);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) CollectRegionTargets(term.else_, targets);
    });
}
std::optional<ir::Location> RegionLeafTarget(const ir::Terminal& terminal) {
    std::optional<ir::Location> result;
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) result = term.next;
    });
    return result;
}

RegionPreparationPlan BuildRegionPreparationPlan(ir::Block* block, ir::HIRFunction* function,
        const RegAlloc& allocation, const Config& config, const FeatureSet& features) {
    RegionPreparationPlan plan;
    plan.options = CaptureRegionPreparationOptions(config, features);
    plan.scratch_inputs = CaptureScratchPreparationInputs(allocation.GetGprs(), features);
    plan.standalone = function ? nullptr : block;
    plan.function = function;
    plan.generation = allocation.GPRLocationGeneration();
    plan.allocation_attempt = allocation.AllocationAttempt();
    const auto capture = [&](ir::HIRBlock* hir, ir::Block* current) {
        RegionBlockCertificate certificate{.hir = hir, .block = current,
                                            .location = current->GetStartLocation().Value(),
                                            .end_location = current->GetEndLocation().Value()};
        if (hir) {
            certificate.predecessor_begin = plan.predecessors.size();
            for (auto* predecessor : hir->GetPredecessors()) plan.predecessors.push_back(predecessor);
            certificate.predecessor_count = plan.predecessors.size() - certificate.predecessor_begin;
            certificate.successor_begin = plan.successors.size();
            for (auto* successor : hir->GetSuccessors()) plan.successors.push_back(successor);
            certificate.successor_count = plan.successors.size() - certificate.successor_begin;
            certificate.call_return = hir->GetCallReturnBlock();
            certificate.call_return_target = hir->IsCallReturnBlock();
        }
        if (const auto* actions = allocation.GPRPlanViewFor(current)) {
            certificate.pinned = actions->pinned;
            certificate.consumers = actions->consumers;
        }
        plan.certificate.push_back(std::move(certificate));
    };
    if (!function) {
        capture(nullptr, block);
        plan.transparent.emplace(block, AnalyzeBlockFlagsTransparent(*block));
        const std::array<ir::Block*, 1> blocks{block};
        plan.unaligned_atomics = BuildUnalignedAtomicFallbackPlan(blocks, allocation,
                plan.options.atomic_lse, plan.options.atomic_memory_base);
        PrepareBackwardTerminalCandidates(plan, block);
        return plan;
    }
    plan.root = function->GetFunction();
    plan.root_location = plan.root->GetStartLocation().Value();
    plan.root_end = plan.root->GetEndLocation().Value();
    plan.all_members = CapturePointers(function->GetHIRBlocks());
    plan.listed_members = CapturePointers(function->GetHIRBlockList());
    plan.rpo = CapturePointers(function->GetHIRBlocksRPO());
    plan.external_roots = function->GetExternalEntryRoots();
    for (const auto& edge : function->GetExternalDirectLinks()) plan.external_links.emplace_back(edge.source, edge.target.Value());
    plan.provenance = function->GetFunctionEntryProvenance();
    // HIR block order IDs index the already captured complete member roster.
    // Reject foreign CFG references rather than certifying only their pointer
    // and later reading unowned IR through canonical-entry analysis.
    const auto owns = [&](const ir::HIRBlock* hir) {
        return !hir || (hir->GetOrderId() < plan.all_members.size() &&
                        plan.all_members[hir->GetOrderId()] == hir);
    };
    const auto closed = [&](const auto& pointers) {
        return std::all_of(pointers.begin(), pointers.end(), [&](const auto* hir) { return hir && owns(hir); });
    };
    plan.valid = closed(plan.listed_members) && closed(plan.rpo) && closed(plan.external_roots);
    for (size_t i = 0; i < plan.all_members.size(); ++i) if (auto* hir = plan.all_members[i])
        plan.valid &= hir->GetOrderId() == i && owns(hir) && closed(hir->GetPredecessors()) &&
                      closed(hir->GetSuccessors()) && owns(hir->GetCallReturnBlock());
    for (const auto& link : plan.external_links) plan.valid &= link.first && owns(link.first);
    if (!plan.valid) return plan;
    plan.certificate.reserve(plan.all_members.size());
    plan.predecessors.reserve(plan.all_members.size() * 2);
    plan.successors.reserve(plan.all_members.size() * 2);
    for (auto* hir : plan.all_members) if (hir) capture(hir, hir->GetBlock());
    plan.canonical_terminal_entries = FunctionEntryContract::AnalyzeCanonicalTerminalEntries(*function);
    std::vector<ir::Block*> blocks;
    for (auto* hir : plan.rpo) {
        auto* current = hir->GetBlock();
        if (current->GetInstList().empty() && !current->HasTerminal()) continue;
        blocks.push_back(current);
        PrepareBackwardTerminalCandidates(plan, current);
        if (plan.options.region_edges) {
            plan.blocks.emplace(current->GetStartLocation().Value(), RegionBlockFacts{.block = current});
        }
    }
    plan.unaligned_atomics = BuildUnalignedAtomicFallbackPlan(blocks, allocation,
            plan.options.atomic_lse, plan.options.atomic_memory_base);
    plan.active = plan.options.region_edges && blocks.size() >= 2;
    if (!plan.active) {
        plan.blocks.clear();
        for (auto* current : blocks)
            plan.transparent.emplace(current, AnalyzeBlockFlagsTransparent(*current));
        return plan;
    }
    bool needs_acceptance = false;
    for (auto& [location, facts] : plan.blocks) {
        facts.edge_flags = AnalyzeBlockEdgeFlags(*facts.block, [&](const ir::Inst& inst) {
            return TargetMayFaultOrObserve(inst, features);
        });
        const bool conditional = !needs_acceptance && plan.options.flags_regs &&
                HasConditionalEdgeTerminal(facts.block->GetTerminal());
        bool writes_flags = false;
        facts.transparent = AnalyzeBlockFlagsTransparent(*facts.block, conditional ? &writes_flags : nullptr);
        needs_acceptance |= writes_flags;
        facts.token_overwritten = plan.options.flags_regs && !facts.transparent &&
                AnalyzeBlockTokenOverwrite(*facts.block, [&](const ir::Inst& inst) {
                    return TargetMayFaultOrObserve(inst, features) ||
                           HelperCallContract::InstructionClobbersGPR(inst, 12, features);
                });
    }
    // Share the existing ordered location roster between acceptance and DFS.
    plan.internal_locations.reserve(plan.blocks.size());
    for (const auto& [location, facts] : plan.blocks) plan.internal_locations.push_back(location);
    const auto& locations = plan.internal_locations;
    if (needs_acceptance) {
        std::vector<EdgeAcceptanceNode> acceptance;
        acceptance.reserve(plan.blocks.size());
        for (const auto& [location, facts] : plan.blocks) {
            EdgeAcceptanceNode node{.accepted = AnalyzeBlockEdgeAcceptance(*facts.block)};
            if (facts.transparent) if (const auto target = RegionLeafTarget(facts.block->GetTerminal())) {
                const auto found = std::lower_bound(locations.begin(), locations.end(), target->Value());
                if (found != locations.end() && *found == target->Value())
                    node.successor = static_cast<u32>(found - locations.begin());
            }
            acceptance.push_back(node);
        }
        const auto accepted = ResolveEdgeAcceptance(acceptance);
        size_t index = 0;
        for (auto& [location, facts] : plan.blocks) facts.accepted_masks = accepted[index++];
    }
    const auto node_index = [&](u64 location) {
        const auto found = std::lower_bound(locations.begin(), locations.end(), location);
        ASSERT(found != locations.end() && *found == location);
        return static_cast<u32>(found - locations.begin());
    };
    std::vector<std::vector<u32>> graph(locations.size());
    std::vector<u64> targets;
    for (auto* current : blocks) {
        auto& successors = graph[node_index(current->GetStartLocation().Value())];
        targets.clear();
        CollectRegionTargets(current->GetTerminal(), targets);
        for (const u64 target : targets) {
            const auto found = std::lower_bound(locations.begin(), locations.end(), target);
            if (found == locations.end() || *found != target) continue;
            const auto index = static_cast<u32>(found - locations.begin());
            if (std::find(successors.begin(), successors.end(), index) == successors.end()) successors.push_back(index);
        }
    }
    enum class Color : u8 { White, Gray, Black };
    std::vector<Color> colors(locations.size(), Color::White);
    std::function<void(u32)> visit = [&](u32 source) {
        colors[source] = Color::Gray;
        for (const u32 target : graph[source]) {
            if (colors[target] == Color::Gray) plan.cycle_edges.emplace(locations[source], locations[target]);
            else if (colors[target] == Color::White) visit(target);
        }
        colors[source] = Color::Black;
    };
    for (auto* current : blocks) {
        const auto source = node_index(current->GetStartLocation().Value());
        if (colors[source] == Color::White) visit(source);
    }
    std::optional<u64> previous_source;
    for (const auto& edge : plan.cycle_edges) if (edge.first != previous_source) {
        ++plan.local_cycle_source_count;
        previous_source = edge.first;
    }
    return plan;
}

std::optional<bool> RegionPreparationPlan::BlockTransparency(const ir::Block* block) const {
    if (active) {
        const auto found = blocks.find(block->GetStartLocation().Value());
        if (found != blocks.end() && found->second.block == block) return found->second.transparent;
    } else {
        const auto found = transparent.find(block);
        if (found != transparent.end()) return found->second;
    }
    return std::nullopt;
}

bool RegionPreparationPlan::MatchesUnit(ir::Block* current_block, ir::HIRFunction* current_function,
        const RegAlloc& allocation, const Config& config, const FeatureSet& features) const {
    if (!valid || !allocation.GPRAllocationComplete() || function != current_function || standalone != current_block ||
        generation != allocation.GPRLocationGeneration() || allocation_attempt != allocation.AllocationAttempt() ||
        !MatchesTargetInputs(allocation, config, features)) return false;
    if (function) {
        if (root != function->GetFunction() || root_location != root->GetStartLocation().Value() ||
            root_end != root->GetEndLocation().Value() ||
            !MatchesPointers(function->GetHIRBlocks(), all_members) ||
            !MatchesPointers(function->GetHIRBlockList(), listed_members) ||
            !MatchesPointers(function->GetHIRBlocksRPO(), rpo) ||
            function->GetExternalEntryRoots() != external_roots ||
            !MatchesProvenance(function->GetFunctionEntryProvenance(), provenance)) return false;
        const auto& links = function->GetExternalDirectLinks();
        if (links.size() != external_links.size()) return false;
        for (size_t i = 0; i < links.size(); ++i)
            if (links[i].source != external_links[i].first || links[i].target.Value() != external_links[i].second) return false;
    }
    for (const auto& proof : certificate) {
        if ((proof.hir && (proof.hir->GetBlock() != proof.block ||
                          !MatchesPointers(proof.hir->GetPredecessors(),
                              std::span<ir::HIRBlock* const>{predecessors}.subspan(proof.predecessor_begin, proof.predecessor_count)) ||
                          !MatchesPointers(proof.hir->GetSuccessors(),
                              std::span<ir::HIRBlock* const>{successors}.subspan(proof.successor_begin, proof.successor_count)) ||
                          proof.hir->GetCallReturnBlock() != proof.call_return ||
                          proof.hir->IsCallReturnBlock() != proof.call_return_target)) ||
            proof.block->GetStartLocation().Value() != proof.location ||
            proof.block->GetEndLocation().Value() != proof.end_location) return false;
        if (proof.pinned) {
            const auto* actions = allocation.GPRPlanViewFor(proof.block);
            if (!actions || actions->pinned != proof.pinned || actions->consumers != proof.consumers ||
                (proof.consumers && !proof.consumers->valid)) return false;
        }
        if (proof.pinned && !proof.pinned->MatchesFrozenIR()) return false;
    }
    return true;
}

bool RegionPreparationPlan::MatchesTargetInputs(const RegAlloc& allocation,
        const Config& config, const FeatureSet& features) const {
    return MatchesScratchInputs(allocation.GetGprs(), features) &&
           options == CaptureRegionPreparationOptions(config, features);
}

bool RegionPreparationPlan::MatchesScratchInputs(const GPRSMask& pool, const FeatureSet& features) const {
    return scratch_inputs == CaptureScratchPreparationInputs(pool, features);
}
} // namespace swift::runtime::backend::arm64
