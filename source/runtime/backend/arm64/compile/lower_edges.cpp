#include "actions.h"
#include "edge_flags.h"
#include "lower_edges.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/function_entry_contract.h"
#include "runtime/common/variant_util.h"
#include "runtime/ir/hir_builder.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {
namespace {
struct CarryRecipe {
    bool canonical{};
    u8 inverted{};
    const ir::Inst* load{};
    const ir::Inst* store{};
    const ir::Inst* marker{};
};
std::optional<CarryRecipe> BuildCarry(const ir::Block* block,
                                      const ir::Inst* final_save,
                                      const ir::Inst* condition,
                                      bool dead_successor,
                                      bool canonical) {
    CarryRecipe recipe{.canonical = canonical, .marker = final_save};
    if (recipe.canonical) {
        for (auto& inst : block->GetInstList()) {
            if (inst.Id() > final_save->Id() && inst.Id() < condition->Id() &&
                inst.GetOp() == ir::OpCode::InvertCarry) {
                recipe.marker = &inst;
            }
        }
        return recipe;
    }

    constexpr u32 kCarryOffset = offsetof(swift::x86::ThreadContext64, carry_inverted);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= final_save->Id() || inst.GetOp() != ir::OpCode::StoreUniform) {
            continue;
        }
        const auto uniform = inst.GetArg<ir::Uniform>(0);
        if (uniform.GetOffset() != kCarryOffset || uniform.GetType() != ir::ValueType::U8) {
            continue;
        }
        auto value = inst.GetArg<ir::Value>(1);
        auto* def = value.Def();
        if (!def || def->GetOp() != ir::OpCode::LoadImm || value.Type() != ir::ValueType::U8 ||
            (!dead_successor && def->GetUses() != 1)) {
            return std::nullopt;
        }
        const u64 immediate = def->GetArg<ir::Imm>(0).Get();
        if (immediate > 1) {
            return std::nullopt;
        }
        recipe.store = &inst;
        recipe.load = def;
        recipe.marker = &inst;
        recipe.inverted = static_cast<u8>(immediate);
    }
    if (!recipe.store || !recipe.load || recipe.store->Id() >= condition->Id()) {
        return std::nullopt;
    }
    return recipe;
}

void AppendBackwardCandidates(EdgeActions& plan, u64 source, std::vector<u64>& targets) {
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (u64 target : targets)
        if (target < source) plan.backward_terminal_candidates.emplace_back(source, target);
}

void PrepareBlockAudit(EdgeBlockAction& row,
                       bool strict_internal,
                       bool count_guest,
                       const FeatureSet& features) {
    if (!strict_internal && !count_guest) return;
    // Keep the former raw-terminal policy: exactly two collected targets,
    // including duplicates, both internal. Only the IR suffix is summarized;
    // the emitter still decides whether NZCV is actually dirty at AdvancePC.
    for (const auto& inst : row.block->GetInstList()) {
        const bool advance = inst.GetOp() == ir::OpCode::AdvancePC;
        if (count_guest && advance) {
            ASSERT(row.guest_instructions < UINT16_MAX);
            ++row.guest_instructions;
        }
        if (!strict_internal) continue;
        if (TargetMayFaultOrObserve(inst, features))
            row.strict_advance_first = UINT16_MAX;
        else if (advance && row.strict_advance_first == UINT16_MAX)
            row.strict_advance_first = inst.Id();
    }
}
}  // namespace

LoweredEdges LowerUnitEdges(const ir::Block* block,
                            const ir::HIRFunction* function,
                            compile::AllocationView allocation,
                            const UnitActions& unit_actions,
                            const Config& config,
                            const FeatureSet& features) {
    LoweredEdges result{
            CaptureUnitCertificate(block, function, allocation, unit_actions, config, features),
            {}};
    if (!result.certificate.valid) return result;
    auto& plan = result.actions;
    const auto& options = result.certificate.options;
    std::vector<const ir::Block*> blocks;
    if (!function)
        blocks.push_back(block);
    else {
        const auto canonical = FunctionEntryContract::AnalyzeCanonicalTerminalEntries(*function);
        plan.canonical_terminal_entries.assign(canonical.begin(), canonical.end());
        std::sort(plan.canonical_terminal_entries.begin(), plan.canonical_terminal_entries.end());
        for (const auto* hir : result.certificate.rpo) {
            const auto* current = hir->GetBlock();
            if (current->GetInstList().empty() && !current->HasTerminal()) continue;
            blocks.push_back(current);
        }
    }
    for (const auto* current : blocks) {
        plan.blocks.push_back({current->GetStartLocation().Value(), current});
    }
    std::stable_sort(plan.blocks.begin(), plan.blocks.end(), [](const auto& a, const auto& b) {
        return a.location < b.location;
    });
    plan.active = function && options.region_edges && blocks.size() >= 2;
    // Reuse one raw-terminal buffer for the unit. Active preparation consumes
    // its original order and duplicate count before backward deduplication.
    std::vector<u64> targets;
    if (!plan.active) {
        for (auto& facts : plan.blocks) {
            facts.transparent = AnalyzeBlockFlagsTransparent(*facts.block);
            PrepareBlockAudit(facts, false, options.alu_split_audit, features);
        }
        for (const auto* current : blocks) {
            targets.clear();
            CollectEdgeTargets(current->GetTerminal(), targets);
            AppendBackwardCandidates(plan, current->GetStartLocation().Value(), targets);
        }
        return result;
    }
    // Match first insertion by RPO for duplicate locations, as the old ordered
    // map did. Inactive transparency still distinguishes actual block identity.
    plan.blocks.erase(
            std::unique(plan.blocks.begin(),
                        plan.blocks.end(),
                        [](const auto& a, const auto& b) { return a.location == b.location; }),
            plan.blocks.end());
    bool needs_acceptance = false;
    for (auto& facts : plan.blocks) {
        facts.edge_flags = AnalyzeBlockEdgeFlags(*facts.block, [&](const ir::Inst& inst) {
            return TargetMayFaultOrObserve(inst, features);
        });
        const bool conditional = !needs_acceptance && options.flags_regs &&
                                 HasConditionalEdgeTerminal(facts.block->GetTerminal());
        bool writes_flags = false;
        facts.transparent =
                AnalyzeBlockFlagsTransparent(*facts.block, conditional ? &writes_flags : nullptr);
        needs_acceptance |= writes_flags;
        facts.token_overwritten =
                options.flags_regs && !facts.transparent &&
                AnalyzeBlockTokenOverwrite(*facts.block, [&](const ir::Inst& inst) {
                    return TargetMayFaultOrObserve(inst, features) ||
                           HelperCallContract::InstructionClobbersGPR(inst, 12, features);
                });
    }
    // Share the existing ordered location roster between acceptance and DFS.
    std::vector<u64> locations;
    locations.reserve(plan.blocks.size());
    for (const auto& facts : plan.blocks) locations.push_back(facts.location);
    if (needs_acceptance) {
        std::vector<EdgeAcceptanceNode> acceptance;
        acceptance.reserve(plan.blocks.size());
        for (const auto& facts : plan.blocks) {
            EdgeAcceptanceNode node{.accepted = AnalyzeBlockEdgeAcceptance(*facts.block)};
            if (facts.transparent)
                if (const auto target = EdgeLeafTarget(facts.block->GetTerminal())) {
                    const auto found =
                            std::lower_bound(locations.begin(), locations.end(), target->Value());
                    if (found != locations.end() && *found == target->Value())
                        node.successor = static_cast<u32>(found - locations.begin());
                }
            acceptance.push_back(node);
        }
        const auto accepted = ResolveEdgeAcceptance(acceptance);
        size_t index = 0;
        for (auto& facts : plan.blocks) facts.accepted_masks = accepted[index++];
    }
    const auto node_index = [&](u64 location) {
        const auto found = std::lower_bound(locations.begin(), locations.end(), location);
        ASSERT(found != locations.end() && *found == location);
        return static_cast<u32>(found - locations.begin());
    };
    std::vector<std::vector<u32>> graph(locations.size());
    for (auto* current : blocks) {
        const auto source = node_index(current->GetStartLocation().Value());
        auto& successors = graph[source];
        targets.clear();
        CollectEdgeTargets(current->GetTerminal(), targets);
        auto& row = plan.blocks[source];
        if (row.block == current) {
            const bool strict = options.flags_regs_audit && targets.size() == 2 &&
                                std::all_of(targets.begin(), targets.end(), [&](u64 target) {
                                    return plan.ContainsInternalBlock(target);
                                });
            PrepareBlockAudit(row, strict, options.alu_split_audit, features);
        }
        for (const u64 target : targets) {
            const auto found = std::lower_bound(locations.begin(), locations.end(), target);
            if (found == locations.end() || *found != target) continue;
            const auto index = static_cast<u32>(found - locations.begin());
            if (std::find(successors.begin(), successors.end(), index) == successors.end())
                successors.push_back(index);
        }
        AppendBackwardCandidates(plan, current->GetStartLocation().Value(), targets);
    }
    enum class Color : u8 { White, Gray, Black };
    std::vector<Color> colors(locations.size(), Color::White);
    std::function<void(u32)> visit = [&](u32 source) {
        colors[source] = Color::Gray;
        for (const u32 target : graph[source]) {
            if (colors[target] == Color::Gray)
                plan.cycle_edges.emplace_back(locations[source], locations[target]);
            else if (colors[target] == Color::White)
                visit(target);
        }
        colors[source] = Color::Black;
    };
    for (auto* current : blocks) {
        const auto source = node_index(current->GetStartLocation().Value());
        if (colors[source] == Color::White) visit(source);
    }
    std::sort(plan.cycle_edges.begin(), plan.cycle_edges.end());
    plan.cycle_edges.erase(std::unique(plan.cycle_edges.begin(), plan.cycle_edges.end()),
                           plan.cycle_edges.end());
    std::optional<u64> previous_source;
    for (const auto& edge : plan.cycle_edges)
        if (edge.first != previous_source) {
            ++plan.local_cycle_source_count;
            previous_source = edge.first;
        }
    if (options.alu_split_audit) {
        std::vector<u64> targets;
        for (const auto& facts : plan.blocks) {
            targets.clear();
            CollectEdgeTargets(facts.block->GetTerminal(), targets);
            ASSERT(plan.report_targets.size() <= UINT32_MAX && targets.size() <= UINT32_MAX &&
                   targets.size() <= UINT32_MAX - plan.report_targets.size());
            CFGReportRow row{facts.location,
                             static_cast<u32>(plan.report_targets.size()),
                             static_cast<u32>(targets.size()),
                             targets.empty()};
            for (const auto target : targets) row.external |= !plan.ContainsInternalBlock(target);
            VisitVariant<void>(facts.block->GetTerminal(), [&](const auto& term) {
                if constexpr (std::is_same_v<std::decay_t<decltype(term)>, ir::terminal::CheckHalt>)
                    row.external = true;
            });
            plan.report.push_back(row);
            plan.report_targets.insert(plan.report_targets.end(), targets.begin(), targets.end());
        }
    }
    return result;
}

bool BackedgeInputs::FlagsDead(u64 location) const {
    return std::find(flags_dead_targets.begin(), flags_dead_targets.end(), location) !=
           flags_dead_targets.end();
}

std::optional<PFAFAction> LowerPFAF(const ir::Block& block,
                                    bool dead_successor,
                                    const ir::Inst* producer,
                                    const ir::Inst* final_save) {
    if (!dead_successor || !producer || !final_save ||
        final_save->GetOp() != ir::OpCode::SaveFlags ||
        final_save->GetArg<ir::Value>(0).Def() != producer ||
        producer->GetOp() != ir::OpCode::Sub ||
        (producer->ReturnType() != ir::ValueType::U8 &&
         producer->ReturnType() != ir::ValueType::U16))
        return std::nullopt;
    const auto requested = final_save->GetArg<ir::Flags>(1);
    if (!True(requested & ir::Flags::NZCV) || !True(requested & ir::Flags::Parity) ||
        !True(requested & ir::Flags::AuxiliaryCarry))
        return std::nullopt;
    PFAFAction plan{.producer = producer->Id(),
                    .width = static_cast<u8>(ir::GetValueSizeByte(producer->ReturnType()))};
    using Operand = PFAFOperand;
    const auto describe = [&](ir::Value value) -> Operand {
        auto* def = value.Def();
        if (!def || ir::GetValueSizeByte(value.Type()) < plan.width) return {};
        if (def->GetOp() == ir::OpCode::LoadImm)
            return {Operand::Kind::Imm, def->GetArg<ir::Imm>(0).Get(), 0};
        if (def->GetOp() == ir::OpCode::GetHostGPR) {
            const auto offset = def->GetArg<ir::Imm>(1).Get();
            if (offset + plan.width > 8) return {};
            return {Operand::Kind::HostGPR, def->GetArg<ir::Imm>(0).Get(), static_cast<u8>(offset)};
        }
        if (def->GetOp() == ir::OpCode::LoadUniform) {
            const auto uniform = def->GetArg<ir::Uniform>(0);
            if (ir::GetValueSizeByte(uniform.GetType()) < plan.width) return {};
            return {Operand::Kind::Uniform, uniform.GetOffset(), 0};
        }
        return {};
    };
    plan.left = describe(producer->GetArg<ir::Value>(0));
    const auto right = producer->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null()) return std::nullopt;
    if (right.GetLeft().IsImm())
        plan.right = {Operand::Kind::Imm, right.GetLeft().imm.Get(), 0};
    else if (right.GetLeft().IsValue())
        plan.right = describe(right.GetLeft().value);
    if (plan.left.kind == Operand::Kind::None || plan.right.kind == Operand::Kind::None)
        return std::nullopt;
    const auto overlaps = [&](const Operand& operand, const ir::Uniform& uniform) {
        const u64 end = u64{uniform.GetOffset()} + ir::GetValueSizeByte(uniform.GetType());
        return operand.kind == Operand::Kind::Uniform && operand.value < end &&
               uniform.GetOffset() < operand.value + plan.width;
    };
    for (const auto& node : block.GetInstList()) {
        if (node.Id() <= producer->Id()) continue;
        if (node.GetOp() == ir::OpCode::StoreUniform) {
            const auto uniform = node.GetArg<ir::Uniform>(0);
            if (overlaps(plan.left, uniform) || overlaps(plan.right, uniform)) return std::nullopt;
        } else if (node.GetOp() == ir::OpCode::SetHostGPR) {
            const auto home = node.GetArg<ir::Imm>(1).Get();
            if ((plan.left.kind == Operand::Kind::HostGPR && plan.left.value == home) ||
                (plan.right.kind == Operand::Kind::HostGPR && plan.right.value == home))
                return std::nullopt;
        }
    }
    return plan;
}

std::optional<BackedgeSelection> LowerBackedge(const ir::Block& current,
                                               const BackedgeInputs& inputs) {
    auto* block = &current;
    if (!inputs.self_enabled && !inputs.region_enabled) {
        return std::nullopt;
    }

    std::optional<ir::Location> then_target;
    std::optional<ir::Location> else_target;
    ir::Value condition{};
    VisitVariant<void>(block->GetTerminal(), [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            condition = term.cond;
            auto direct = [](const ir::Terminal& edge) -> std::optional<ir::Location> {
                return VisitVariant<std::optional<ir::Location>>(
                        edge, [](const auto& target) -> std::optional<ir::Location> {
                            using E = std::decay_t<decltype(target)>;
                            if constexpr (std::is_same_v<E, ir::terminal::LinkBlock> ||
                                          std::is_same_v<E, ir::terminal::LinkBlockFast>) {
                                return target.next;
                            }
                            return std::nullopt;
                        });
            };
            then_target = direct(term.then_);
            else_target = direct(term.else_);
        }
    });
    if (!then_target || !else_target || !condition.Def() ||
        condition.Def()->GetOp() != ir::OpCode::LocalCondSet) {
        return std::nullopt;
    }
    const auto self = block->GetStartLocation();
    const bool then_self = *then_target == self;
    const bool else_self = *else_target == self;
    const bool then_dead = inputs.region_enabled && inputs.FlagsDead(then_target->Value());
    const bool else_dead = inputs.region_enabled && inputs.FlagsDead(else_target->Value());
    const bool dead_successor = then_dead != else_dead;
    if (!dead_successor && (!inputs.self_enabled || then_self == else_self)) {
        return std::nullopt;
    }

    const ir::Inst* first_producer = nullptr;
    const ir::Inst* final_save = nullptr;
    ir::Flags final_requested{};
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::SaveFlags) {
            continue;
        }
        auto* producer = inst.GetArg<ir::Value>(0).Def();
        if (!producer) {
            return std::nullopt;
        }
        if (!first_producer || producer->Id() < first_producer->Id()) {
            first_producer = producer;
        }
        const auto requested = inst.GetArg<ir::Flags>(1);
        if (True(requested & ir::Flags::Parity) && True(requested & ir::Flags::AuxiliaryCarry) &&
            True(requested & ir::Flags::NZCV)) {
            final_save = &inst;
            final_requested = requested;
        }
    }
    if (!first_producer || !final_save) {
        return std::nullopt;
    }

    auto carry_recipe =
            BuildCarry(block, final_save, condition.Def(), dead_successor, inputs.canonical_carry);
    if (!carry_recipe) {
        return std::nullopt;
    }

    if (!dead_successor) {
        // The old block-entry flags stay live until the first producer. A
        // fault is allowed in that prefix; the self-recipe recovery veneer
        // reconstructs them.
        for (auto& inst : block->GetInstList()) {
            if (&inst == first_producer) {
                break;
            }
            if (!TargetRetainsPendingHostNZCV(inst, inputs.features)) {
                return std::nullopt;
            }
        }
    }
    // Once the next producer overwrites host NZCV there may be no synchronous
    // fault or architectural observer before the terminal safepoint.
    auto* lazy_producer = dead_successor ? final_save->GetArg<ir::Value>(0).Def() : first_producer;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= lazy_producer->Id()) {
            continue;
        }
        const auto helper = HelperCallContract::Resolve(inst, inputs.features);
        if (TargetMayFaultOrObserve(inst, inputs.features) ||
            (helper && !helper->RetainsPendingNZCV())) {
            return std::nullopt;
        }
    }
    // The tail after the omitted store is intentionally tiny: advancing the
    // guest PC and consuming the already-live local condition only. A final
    // branch boundary may follow that condition as the last instruction.
    const ir::Inst* final_advance = nullptr;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= carry_recipe->marker->Id()) {
            continue;
        }
        if (inst.GetOp() != ir::OpCode::AdvancePC && inst.GetOp() != ir::OpCode::LocalCondSet &&
            inst.GetOp() != ir::OpCode::ZeroExtend32 &&
            inst.GetOp() != ir::OpCode::ZeroExtend32To64 &&
            inst.GetOp() != ir::OpCode::SetHostGPR) {
            return std::nullopt;
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            if (!final_advance) {
                final_advance = &inst;
            } else if (&inst != &block->GetInstList().back() ||
                       inst.Id() <= condition.Def()->Id()) {
                return std::nullopt;
            }
        }
    }
    if (!final_advance || final_advance->Id() >= condition.Def()->Id()) {
        return std::nullopt;
    }

    BackedgeSelection selection;
    auto& recipe = selection.action;
    recipe.dead_successor = dead_successor;
    recipe.canonical_carry = carry_recipe->canonical;
    recipe.self_is_then = dead_successor ? then_dead : then_self;
    recipe.self_target = dead_successor ? (then_dead ? *then_target : *else_target) : self;
    recipe.cold_target = recipe.self_is_then ? *else_target : *then_target;
    recipe.carry_inverted = carry_recipe->inverted;
    recipe.requested_mask = static_cast<u8>(final_requested & ir::Flags::NZCV);
    recipe.omit_polarity_load = carry_recipe->load && carry_recipe->load->GetUses() == 1;
    recipe.omit_polarity_store = carry_recipe->store != nullptr;
    if (recipe.omit_polarity_load) recipe.polarity_load = carry_recipe->load->Id();
    if (recipe.omit_polarity_store) recipe.polarity_store = carry_recipe->store->Id();
    recipe.final_advance = final_advance->Id();
    if (const auto pfaf = LowerPFAF(*block, recipe.dead_successor, lazy_producer, final_save))
        selection.pfaf.emplace(*pfaf);

    return selection;
}

}  // namespace swift::runtime::backend::arm64
