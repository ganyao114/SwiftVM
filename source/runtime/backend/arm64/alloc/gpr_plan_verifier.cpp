#include "gpr_plan_verifier.h"
#include "runtime/common/perf_stats.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/common/variant_util.h"
#include <algorithm>
#include <bit>
#include <unordered_map>
#include <unordered_set>

namespace swift::runtime::backend::arm64 {
namespace {
void CollectFlagsOnlySpillUse(ir::Inst& definition, const GPRPlanVerificationInput& input,
                             const RegAlloc& allocation) {
    if (!input.flags_only_spills || !input.analysis || !definition.HasValue() ||
        !definition.HasPseudoOperations() || definition.ReturnType() < ir::ValueType::U8 ||
        definition.ReturnType() > ir::ValueType::S64 || !input.analysis->Contains(&definition)) return;
    // Raw records group all occurrences for one consumer, including Lambda/Params.
    const auto uses = input.analysis->Uses(&definition);
    if (uses.size() != 1) return;
    const auto& use = uses.front();
    if (!input.analysis->Contains(use.consumer) ||
        (use.consumer->GetOp() != ir::OpCode::SaveFlags &&
         use.consumer->GetOp() != ir::OpCode::BranchOnlyFlags) ||
        definition.GetUses(false) != use.count) return;
    const ir::Value value{&definition};
    if (value.Id() >= allocation.MapCount() || allocation.ValueType(value) != RegAlloc::MEM) return;
    if (allocation.GPRLocationAt(value, definition.Id(), RegAlloc::GPRView::WholeValue).kind != RegAlloc::MEM) return;
    input.flags_only_spills->uses.push_back({use.consumer, definition.Id()});
}
} // namespace

// Production path: only the load-bearing outputs run -- width certificates
// (consumed by emission elisions) and flags-only spill collection (consumed
// by JitContext). The fixed-constraint recheck, dominator construction, and
// use-location audit in VerifyGPRAllocationPlan are pure verification and
// run only when the caller opts in via SVM_VERIFY_PLAN.
void VerifyGPRWidthElisionsOnly(std::span<const GPRPlanVerificationInput> inputs,
                                const RegAlloc& allocation, const FeatureSet& features,
                                GPRWidthCertificates* certificates, const GPRSMask& uniforms) {
    for (const auto& input : inputs) {
        PerfScope2 perf_width{GetPerfStats2().verify_width};
        const auto widths = VerifyGPRWidthElisions(input.block, allocation, features, uniforms, input.analysis);
        perf_width.Stop();
        if (certificates) {
            certificates->width.insert(certificates->width.end(), widths.width.begin(), widths.width.end());
            certificates->low32.insert(certificates->low32.end(), widths.low32.begin(), widths.low32.end());
            certificates->reads.insert(certificates->reads.end(), widths.reads.begin(), widths.reads.end());
        }
        if (input.flags_only_spills) {
            for (auto& inst : input.block->GetInstList()) {
                CollectFlagsOnlySpillUse(inst, input, allocation);
            }
            if (input.flags_only_spills->uses.size() > 1) {
                input.flags_only_spills->Finalize();
            }
        }
    }
}

void VerifyGPRAllocationPlan(std::span<const GPRPlanVerificationInput> inputs,
                              const RegAlloc& allocation, const FeatureSet& features,
                              ir::HIRFunction* function, bool internal_edges,
                              GPRWidthCertificates* certificates, const GPRSMask& uniforms, bool fresh_snapshots) {
    // Instruction ids are dense within the unit (MapCount entries);
    // a direct table replaces a per-instruction hash insert/lookup.
    std::vector<ir::Block*> owners(allocation.MapCount());
    std::unordered_map<ir::Block*, ir::HIRBlock*> hir;
    std::unordered_map<ir::HIRBlock*, size_t> block_ids;
    std::vector<std::vector<u64>> dominators;
    std::array<std::vector<u32>, 32> clobbers;
    for (const auto& input : inputs) {
        ASSERT(input.block && input.plan && (fresh_snapshots ? input.plan->MatchesAllocation() : input.plan->MatchesFrozenIR()));
        {
            PerfScope2 perf_fixed{GetPerfStats2().verify_fixed};
            VerifyFixedGPRConstraints(input.block, allocation, features, input.plan->PhysicalWrites());
        }
        PerfScope2 perf_width{GetPerfStats2().verify_width};
        const auto widths = VerifyGPRWidthElisions(input.block, allocation, features, uniforms, input.analysis);
        perf_width.Stop();
        if (certificates) {
            certificates->width.insert(certificates->width.end(), widths.width.begin(), widths.width.end());
            certificates->low32.insert(certificates->low32.end(), widths.low32.begin(), widths.low32.end());
            certificates->reads.insert(certificates->reads.end(), widths.reads.begin(), widths.reads.end());
        }
        for (auto& inst : input.block->GetInstList()) {
            if (inst.Id() < owners.size()) owners[inst.Id()] = input.block;
            if (inst.GetOp() != ir::OpCode::BindLabel)
                ASSERT_MSG(inst.HasValidTypeViews(), "invalid type view: {}", inst);
            CollectFlagsOnlySpillUse(inst, input, allocation);
            u32 mask = FixedGPRClobbers(inst, features);
            if (const auto helper = HelperCallContract::Resolve(inst, features)) {
                // Ordinary SSA residences are saved/restored by EmitHostCall
                // using this instruction's verified live mask. Pinned home
                // versions use the independent fixed-home/helper check above.
                mask = 0;
            }
            while (mask) {
                clobbers[std::countr_zero(mask)].push_back(inst.Id());
                mask &= mask - 1;
            }
            if (!allocation.HasGPRPreservationActions()) continue;
            for (const auto& copy : allocation.GPRPreservationsBefore(inst.Id())) {
                ASSERT(copy.width && copy.width <= 8 && copy.bit_offset + copy.width * 8 <= 64);
                ASSERT(copy.destination_kind == RegAlloc::GPR || copy.destination_kind == RegAlloc::MEM);
                if (copy.destination_kind == RegAlloc::MEM) {
                    ASSERT(copy.destination < kMaxSpillSlots);
                    if (copy.width != 8 || copy.bit_offset) {
                        const auto* reload = allocation.SpillReloadAt(copy.value, inst.Id());
                        ASSERT(copy.scratch < 31 && (!allocation.DirtyGPR(inst.Id()).Get(copy.scratch) ||
                               (reload && reload->reg == copy.scratch)));
                    }
                } else ASSERT(allocation.DirtyGPR(inst.Id()).Get(copy.destination));
            }
        }
        for (const auto& operand : input.plan->Operands()) {
            ASSERT(operand.value.Defined() && operand.type == operand.value.Type());
            for (unsigned view = 0; view < 4; ++view) {
                const auto location = input.plan->UseLocation(operand.value, operand.consumer,
                        static_cast<RegAlloc::GPRView>(view));
                if (location.kind == RegAlloc::GPR) {
                    ASSERT_MSG(location.slot < 31 && (location.bits == 32 || location.bits == 64),
                               "invalid GPR location for SSA {} at IR {}: slot={} bits={}",
                               operand.value.Id(), operand.consumer->Id(), location.slot, location.bits);
                    ASSERT(location.extension.known_zero_above <= 64);
                } else if (location.kind == RegAlloc::MEM) {
                    ASSERT(location.slot < kMaxSpillSlots);
                    ASSERT(location.reload_register == UINT16_MAX || location.reload_register < 31);
                }
            }
        }
        if (input.flags_only_spills && input.flags_only_spills->uses.size() > 1)
            input.flags_only_spills->Finalize();
    }
    for (auto& points : clobbers) std::sort(points.begin(), points.end());
    if (function) {
        for (auto& block : function->GetHIRBlocksRPO()) {
            block_ids.emplace(&block, block_ids.size());
            hir.emplace(block.GetBlock(), &block);
        }
        const size_t words = (block_ids.size() + 63) / 64;
        dominators.assign(block_ids.size(), std::vector<u64>(words, ~u64{}));
        std::unordered_set<ir::HIRBlock*> roots{function->GetEntryBlock()};
        for (auto* root : function->GetExternalEntryRoots()) roots.insert(root);
        bool changed = true;
        std::vector<u64> next(words);
        while (changed) {
            changed = false;
            for (auto& block : function->GetHIRBlocksRPO()) {
                const auto id = block_ids.at(&block);
                std::fill(next.begin(), next.end(), roots.contains(&block) ? u64{} : ~u64{});
                bool predecessor = false;
                if (!roots.contains(&block)) {
                    for (auto& edge : block.GetIncomingEdges()) {
                        const auto found = block_ids.find(edge.src_block);
                        if (found == block_ids.end()) continue;
                        predecessor = true;
                        for (size_t word = 0; word < words; ++word) next[word] &= dominators[found->second][word];
                    }
                    if (!predecessor) std::fill(next.begin(), next.end(), 0);
                }
                next[id / 64] |= u64{1} << (id % 64);
                if (next != dominators[id]) { dominators[id] = next; changed = true; }
            }
        }
    }
    const auto dominates = [&](ir::Block* definition, ir::Block* use) {
        if (definition == use) return true;
        if (!function || !hir.contains(definition) || !hir.contains(use)) return false;
        const auto definition_id = block_ids.at(hir.at(definition));
        const auto use_id = block_ids.at(hir.at(use));
        return (dominators[use_id][definition_id / 64] & (u64{1} << (definition_id % 64))) != 0;
    };
    const auto verify_use = [&](ir::Value value, const ir::Inst* consumer, ir::Block* block, u32 point,
                                const FrozenGPRPlan& plan) {
        if (!value.Defined() || value.Type() == ir::ValueType::VOID) return;
        ir::Block* const owner_block =
                value.Id() < owners.size() ? owners[value.Id()] : nullptr;
        ASSERT_MSG(owner_block && dominates(owner_block, block),
                   "GPR SSA definition {} does not dominate use {}", value.Id(), point);
        ASSERT_MSG(owner_block == block || internal_edges,
                   "cross-block GPR SSA {} requires internal function edges", value.Id());
        if (owner_block != block && function) {
            for (auto* root : function->GetExternalEntryRoots()) {
                // A separately published entry must not need a value that was
                // captured only on another entry path.
                ASSERT_MSG(root != hir.at(block) || dominates(owner_block, root->GetBlock()),
                           "external entry requires unavailable GPR SSA {}", value.Id());
            }
        }
        const auto location = plan.UseLocation(value, consumer, RegAlloc::GPRView::WholeValue);
        if (location.kind != RegAlloc::GPR || location.pinned || allocation.GetGprs().Get(location.slot)) return;
        u32 birth = allocation.AllocationId(value);
        if (const auto* capture = allocation.PreservedGPRDefinitionAt(value.Id())) birth = capture->transfer;
        const auto& points = clobbers[location.slot];
        const auto next = std::upper_bound(points.begin(), points.end(), birth);
        ASSERT_MSG(next == points.end() || *next >= point,
                   "GPR SSA {} in x{} crosses physical clobber {} before use {}",
                   value.Id(), location.slot, next == points.end() ? UINT32_MAX : *next, point);
    };
    for (const auto& input : inputs) {
        for (auto& inst : input.block->GetInstList()) {
            if (inst.IsPseudoOperation() || inst.GetOp() == ir::OpCode::BindLabel) continue;
            for (auto value : inst.GetValues()) verify_use(value, &inst, input.block, inst.Id(), *input.plan);
        }
        const auto* tail = input.block->IsEmptyBlock() ? nullptr : &input.block->GetInstList().back();
        std::function<void(const ir::Terminal&)> walk;
        walk = [&](const ir::Terminal& tree) {
            VisitVariant<void>(tree, [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, ir::terminal::If>) {
                    verify_use(node.cond, tail, input.block, tail ? tail->Id() : 0, *input.plan);
                    walk(node.then_); walk(node.else_);
                } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                    verify_use(node.value, tail, input.block, tail ? tail->Id() : 0, *input.plan);
                    for (const auto& item : node.cases) walk(item.then);
                } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                    walk(node.then_); walk(node.else_);
                } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) walk(node.else_);
            });
        };
        walk(input.block->GetTerminal());
    }
}
}
