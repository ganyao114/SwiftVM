#include "backedge_flags_plan.h"
#include "target_analysis_traits.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/common/variant_util.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {
namespace {
struct CarryRecipe { bool canonical{}; u8 inverted{}; ir::Inst* load{}; ir::Inst* store{}; ir::Inst* marker{}; };
std::optional<CarryRecipe>
BuildCarry(ir::Block* block, ir::Inst* final_save,
                                 ir::Inst* condition, bool dead_successor, bool canonical) {
    CarryRecipe recipe{.canonical = canonical,
                           .marker = final_save};
    if (recipe.canonical) {
        for (auto& inst : block->GetInstList()) {
            if (inst.Id() > final_save->Id() &&
                inst.Id() < condition->Id() &&
                inst.GetOp() == ir::OpCode::InvertCarry) {
                recipe.marker = &inst;
            }
        }
        return recipe;
    }

    constexpr u32 kCarryOffset = offsetof(swift::x86::ThreadContext64,
                                          carry_inverted);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= final_save->Id() ||
            inst.GetOp() != ir::OpCode::StoreUniform) {
            continue;
        }
        const auto uniform = inst.GetArg<ir::Uniform>(0);
        if (uniform.GetOffset() != kCarryOffset ||
            uniform.GetType() != ir::ValueType::U8) {
            continue;
        }
        auto value = inst.GetArg<ir::Value>(1);
        auto* def = value.Def();
        if (!def || def->GetOp() != ir::OpCode::LoadImm ||
            value.Type() != ir::ValueType::U8 ||
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

} // namespace

std::optional<BackedgeFlagsPlan>
BuildBackedgeFlagsPlan(ir::Block& current, const BackedgeFlagsInputs& inputs) {
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
    const bool then_dead = inputs.region_enabled && inputs.flags_dead_targets.contains(then_target->Value());
    const bool else_dead = inputs.region_enabled && inputs.flags_dead_targets.contains(else_target->Value());
    const bool dead_successor = then_dead != else_dead;
    if (!dead_successor && (!inputs.self_enabled || then_self == else_self)) {
        return std::nullopt;
    }

    ir::Inst* first_producer = nullptr;
    ir::Inst* final_save = nullptr;
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
        if (True(requested & ir::Flags::Parity) &&
            True(requested & ir::Flags::AuxiliaryCarry) &&
            True(requested & ir::Flags::NZCV)) {
            final_save = &inst;
            final_requested = requested;
        }
    }
    if (!first_producer || !final_save) {
        
        return std::nullopt;
    }

    auto carry_recipe = BuildCarry(
            block, final_save, condition.Def(), dead_successor, inputs.canonical_carry);
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
    auto* lazy_producer = dead_successor
            ? final_save->GetArg<ir::Value>(0).Def()
            : first_producer;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= lazy_producer->Id()) {
            continue;
        }
        const auto helper = HelperCallContract::Resolve(inst, inputs.features);
        if (TargetMayFaultOrObserve(inst, inputs.features) || (helper && !helper->RetainsPendingNZCV())) {
            
            return std::nullopt;
        }
    }
    // The tail after the omitted store is intentionally tiny: advancing the
    // guest PC and consuming the already-live local condition only.
    ir::Inst* final_advance = nullptr;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= carry_recipe->marker->Id()) {
            continue;
        }
        if (inst.GetOp() != ir::OpCode::AdvancePC &&
            inst.GetOp() != ir::OpCode::LocalCondSet &&
            inst.GetOp() != ir::OpCode::ZeroExtend32 &&
            inst.GetOp() != ir::OpCode::ZeroExtend32To64 &&
            inst.GetOp() != ir::OpCode::SetHostGPR) {
            
            return std::nullopt;
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            if (final_advance) {
                return std::nullopt;
            }
            final_advance = &inst;
        }
    }
    if (!final_advance || final_advance->Id() >= condition.Def()->Id()) {
        return std::nullopt;
    }

    BackedgeFlagsPlan recipe;
    recipe.dead_successor = dead_successor;
    recipe.canonical_carry = carry_recipe->canonical;
    recipe.self_is_then = dead_successor ? then_dead : then_self;
    recipe.self_target = dead_successor
            ? (then_dead ? *then_target : *else_target)
            : self;
    recipe.cold_target = recipe.self_is_then ? *else_target : *then_target;
    recipe.carry_inverted = carry_recipe->inverted;
    recipe.requested = final_requested & ir::Flags::NZCV;
    recipe.polarity_load = carry_recipe->load && carry_recipe->load->GetUses() == 1
            ? carry_recipe->load
            : nullptr;
    recipe.polarity_store = carry_recipe->store;
    recipe.final_save = final_save;
    recipe.final_advance = final_advance;
    if (const auto pfaf = BuildRegionPFAFPlan(*block, recipe.dead_successor, lazy_producer, recipe.final_save))
        recipe.pfaf.emplace(*pfaf);

    return recipe;
}



}
