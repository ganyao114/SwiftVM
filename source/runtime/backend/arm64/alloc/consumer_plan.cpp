#include "consumer_plan.h"
#include <ranges>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::arm64 {
namespace {
bool IsAluSplitOpcode(ir::OpCode op) {
    using O = ir::OpCode;
    return op == O::Sub || op == O::And || op == O::Or || op == O::Xor;
}

bool IsWidthBridgeOpcode(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::BitCast:
        case O::ZeroExtend32:
        case O::ZeroExtend32To64:
        case O::ZeroExtend64:
        case O::BitExtract:
            return true;
        default:
            return false;
    }
}

bool DataIsDef(const ir::Operand::Type& data, ir::Inst* def) {
    return data.IsValue() && data.value.Def() == def;
}

bool InstRefsDef(ir::Inst* user, ir::Inst* def) {
    const auto op = user->GetOp();
    if (op == ir::OpCode::StoreMemory || op == ir::OpCode::StoreMemoryTSO) {
        const auto addr = user->GetArg<ir::Operand>(0);
        const auto data = user->GetArg<ir::Value>(1);
        return DataIsDef(addr.GetLeft(), def) || DataIsDef(addr.GetRight(), def) ||
               data.Def() == def;
    }
    if (op == ir::OpCode::LoadMemory || op == ir::OpCode::LoadMemoryTSO ||
        op == ir::OpCode::GetOperand) {
        const auto addr = user->GetArg<ir::Operand>(0);
        return DataIsDef(addr.GetLeft(), def) || DataIsDef(addr.GetRight(), def);
    }
    bool found = false;
    user->ForEachValue([&](ir::Value value) { found |= value.Def() == def; });
    return found;
}

bool IsAddressUse(ir::Inst* user, ir::Inst* def) {
    using O = ir::OpCode;
    const auto op = user->GetOp();
    if (op == O::GetOperand || op == O::LoadMemory || op == O::LoadMemoryTSO) {
        return true;
    }
    if (op == O::StoreMemory || op == O::StoreMemoryTSO) {
        return user->GetArg<ir::Value>(1).Def() != def;
    }
    if (op == O::AtomicExchange || op == O::AtomicFetchAdd) {
        return user->GetArg<ir::Value>(0).Def() == def &&
               user->GetArg<ir::Value>(1).Def() != def;
    }
    if (op == O::AtomicRMW) {
        return user->GetArg<ir::Value>(1).Def() == def;
    }
    return false;
}

AluSplitFacts BuildAluSplitFacts(const BlockAnalysisIndex& analysis, ir::Inst* inst) {
    AluSplitFacts out;
    out.value_uses = inst->GetUses(true);
    std::array<ir::Inst*, 32> family{};
    u32 family_n = 1;
    family[0] = inst;
    bool grew = true;
    while (grew) {
        grew = false;
        for (auto* user : analysis.Instructions()) {
            if (!IsWidthBridgeOpcode(user->GetOp()) || family_n >= family.size()) {
                continue;
            }
            bool already = false;
            for (u32 i = 0; i < family_n; ++i) {
                if (family[i] == user) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            auto src = user->GetArg<ir::Value>(0).Def();
            for (u32 i = 0; i < family_n; ++i) {
                if (family[i] == src) {
                    family[family_n++] = user;
                    grew = true;
                    break;
                }
            }
        }
    }

    bool saw_addr = false;
    bool saw_value = false;
    u32 found = 0;
    for (auto* user : analysis.Instructions()) {
        bool in_family = false;
        for (u32 i = 0; i < family_n; ++i) {
            if (family[i] == user) {
                in_family = true;
                break;
            }
        }
        if (in_family) {
            continue;
        }
        ir::Inst* which = nullptr;
        for (u32 i = 0; i < family_n; ++i) {
            if (InstRefsDef(user, family[i])) {
                which = family[i];
                break;
            }
        }
        if (!which) {
            continue;
        }
        ++found;
        if (IsAddressUse(user, which)) {
            saw_addr = true;
        } else {
            saw_value = true;
        }
    }
    out.family_count = static_cast<u8>(family_n);
    out.family_capped = family_n == family.size();
    out.external_users = found;
    out.saw_address = saw_addr;
    out.saw_value = saw_value;
    return out;
}

bool BuildPairResults(ConsumerPlan& plan, const BlockAnalysisIndex& analysis) {
    const auto nodes = analysis.Instructions();
    for (auto* producer : nodes) {
        const auto op = producer->GetOp();
        if (op != ir::OpCode::Div128 && op != ir::OpCode::Cpuid) continue;
        if (producer->ReturnType() != ir::ValueType::U64) return false;
        ConsumerPlan::PairResultAction action{
            op == ir::OpCode::Div128 ? ir::OpCode::Div128Remainder : ir::OpCode::CpuidUpper};
        u32 local_pseudo_uses = 0;
        for (const auto& use : analysis.Uses(producer)) {
            auto* pseudo = use.consumer;
            if (!pseudo->IsPseudoOperation()) continue;
            local_pseudo_uses += use.count;
            // A pair secondary must consume its producer exactly once,
            // as operand 0 (a malformed double use is rejected here,
            // replacing the old pseudo-chain cycle guard).
            if (use.count != 1 || !analysis.Contains(pseudo) ||
                !pseudo->ArgAt(0).IsValue() ||
                pseudo->GetArg<ir::Value>(0).Def() != producer || pseudo->Id() <= producer->Id())
                return false;
            const auto secondary = pseudo->GetOp();
            if (secondary != ir::OpCode::Div128Remainder && secondary != ir::OpCode::CpuidUpper) continue;
            if (secondary != action.secondary_opcode || action.secondary ||
                pseudo->ReturnType() != ir::ValueType::U64 ||
                pseudo->GetArg<ir::Value>(0).Type() != ir::ValueType::U64)
                return false;
            action.secondary = pseudo;
        }
        // A pseudo use from another block is invisible to this
        // block's index, but the producer's use counters still see
        // it; any unaccounted pseudo use is foreign ownership.
        if (producer->GetUses(false) - producer->GetUses() != local_pseudo_uses)
            return false;
        plan.pair_results.emplace(producer, action);
    }
    // A local secondary must also be reachable from its admitted local owner.
    for (auto* node : nodes) {
        const auto op = node->GetOp();
        if (op != ir::OpCode::Div128Remainder && op != ir::OpCode::CpuidUpper) continue;
        if (!node->ArgAt(0).IsValue()) return false;
        const auto owner = plan.pair_results.find(node->GetArg<ir::Value>(0).Def());
        if (owner == plan.pair_results.end() || owner->second.secondary != node) return false;
    }
    return true;
}

ConsumerPlan::FCmpAction BuildFCmpAction(ConsumerPlan& plan, const BlockAnalysisIndex& analysis,
        ir::Inst* producer, const std::function<bool(const ir::Inst&)>& retains_nzcv,
        const std::function<bool(const ir::Inst&)>& observes) {
    ConsumerPlan::FCmpAction action;
    const auto uses = analysis.Uses(producer);
    const bool compact = producer->GetArg<ir::Imm>(3).Get() != 0;
    if (uses.empty()) {
        // A named relation crossing a block edge cannot borrow this block's
        // host NZCV. Its receiving plan consumes the full SSA relation.
        action.packed_relation = compact && producer->GetUses(false) != 0;
        return action;
    }
    const bool sole_condition = producer->GetUses() == 1 && uses.size() == 1 && uses.front().count == 1 &&
            uses.front().consumer->GetOp() == ir::OpCode::FCmpCondSet;
    u32 accounted = 0;
    bool carrier = true;
    for (const auto& use : uses) {
        accounted += use.count;
        const auto op = use.consumer->GetOp();
        if (op != ir::OpCode::PublishFCmpFlags && op != ir::OpCode::FCmpCondSet) {
            carrier = false;
            action.packed_relation |= compact;
        }
    }
    if (accounted != producer->GetUses(false)) {
        carrier = false;
        action.packed_relation |= compact;
    }
    const auto nodes = analysis.Instructions();
    const auto first = std::upper_bound(nodes.begin(), nodes.end(), producer->Id(),
            [](u32 id, const ir::Inst* node) { return id < node->Id(); });
    bool raw_safe = true, compact_safe = true, published = false;
    unsigned readers = 0;
    StackVector<ir::Inst*, 2> raw_readers, inversions;
    // One ordered window establishes all readers, publication eligibility,
    // carrier ownership and inversion snapshots for this named relation.
    for (auto at = first; at != nodes.end() && (*at)->Id() <= uses.back().consumer->Id(); ++at) {
        auto* node = *at;
        const auto op = node->GetOp();
        if (op == ir::OpCode::PublishFCmpFlags && node->GetArg<ir::Value>(0).Def() == producer) {
            if (compact && (!raw_safe || published)) action.packed_relation = true;
            carrier &= at == first && node->GetArg<ir::Imm>(1).Get() != 0 && !published;
            published = true;
            continue;
        }
        if (op == ir::OpCode::FCmpCondSet) {
            if (node->GetArg<ir::Value>(0).Def() == producer) {
                ++readers;
                if (sole_condition && raw_safe && !published) action.raw_condition = node;
                else if (raw_safe) raw_readers.push_back(node);
                const auto cond = node->GetArg<ir::Cond>(1);
                const bool supported = cond == ir::Cond::LT || cond == ir::Cond::GE || cond == ir::Cond::GT ||
                        cond == ir::Cond::LE || cond == ir::Cond::VS || cond == ir::Cond::VC;
                if (compact && node != action.raw_condition && (!published || !compact_safe || !supported))
                    action.packed_relation = true;
                carrier &= published && compact_safe && readers == 1;
                if (compact && !inversions.empty())
                    plan.carry_inversions[node].assign(inversions.begin(), inversions.end());
            }
            continue; // All relation-reader encodings preserve host NZCV.
        }
        if (op == ir::OpCode::InvertCarry) {
            if (compact) inversions.push_back(node);
            raw_safe = false;
            if (!published) compact_safe = false;
            continue;
        }
        if (analysis.HasPseudoUse(node) || !retains_nzcv(*node) || (!published && observes(*node))) {
            raw_safe = false;
            compact_safe = false;
        }
    }
    action.compact_carrier = compact && !action.packed_relation && carrier && published &&
            uses.size() == producer->GetUses();
    if (!compact || action.packed_relation) {
        action.raw_all_conditions = readers && raw_readers.size() == readers;
        if (!action.raw_all_conditions)
            plan.raw_fcmp_conditions.insert(raw_readers.begin(), raw_readers.end());
    }
    return action;
}
}
ConsumerPlan BuildConsumerPlan(const BlockAnalysisIndex& analysis,
        const std::function<bool(const ir::Inst&)>& retains_nzcv,
        const std::function<bool(const ir::Inst&)>& observes,
        const ir::Terminal* terminal, bool alu_split_audit) {
    ConsumerPlan plan;
    plan.valid = BuildPairResults(plan, analysis);
    if (!plan.valid) return plan;
    if (alu_split_audit)
        for (auto* inst : analysis.Instructions())
            if (IsAluSplitOpcode(inst->GetOp()))
                plan.alu_split.emplace(inst, BuildAluSplitFacts(analysis, inst));
    std::unordered_map<const ir::Inst*, bool> terminal_conditions;
    bool terminal_pstate = false;
    const auto visit = [&](auto&& self, const ir::Terminal& node, bool pstate_reloaded) -> void {
        VisitVariant<void>(node, [&](const auto& edge) {
            using T = std::decay_t<decltype(edge)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                if (edge.cond.Def()) {
                    auto condition = terminal_conditions.try_emplace(edge.cond.Def(), !pstate_reloaded).first;
                    condition->second &= !pstate_reloaded;
                }
                self(self, edge.then_, pstate_reloaded); self(self, edge.else_, pstate_reloaded);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                terminal_pstate = true;
                // Guest-condition evaluation may reload canonical NZCV. A
                // descendant raw predicate must retain its earlier SSA value.
                self(self, edge.then_, true); self(self, edge.else_, true);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) self(self, edge.else_, pstate_reloaded);
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>)
                for (const auto& arm : edge.cases) self(self, arm.then, pstate_reloaded);
        });
    };
    if (terminal) visit(visit, *terminal, false);
    const auto nodes = analysis.Instructions();
    const auto can_defer_local = [&](const ir::Inst* condition, u32 before) {
        // These readers capture host NZCV at their own IR point. Keeping only
        // a condition code must prove the second window through the user too.
        if (condition->GetOp() != ir::OpCode::FCmpCondSet && condition->GetOp() != ir::OpCode::LocalCondSet)
            return true;
        for (auto* node : analysis.Between(condition->Id(), before))
            if (analysis.HasPseudoUse(node) || !retains_nzcv(*node) || observes(*node)) return false;
        return true;
    };
    for (size_t i = 1; i + 1 < nodes.size(); ++i) {
        auto* clear = nodes[i];
        if (clear->GetOp() != ir::OpCode::ClearFlags ||
            clear->GetArg<ir::Flags>(0) != (ir::Flags::CV | ir::Flags::AuxiliaryCarry)) continue;
        auto* producer = nodes[i - 1];
        auto* save = nodes[i + 1];
        if (save->GetOp() != ir::OpCode::SaveFlags || save->GetArg<ir::Value>(0).Def() != producer ||
            save->GetArg<ir::Flags>(1) != (ir::Flags::NZ | ir::Flags::Parity)) continue;
        const auto op = producer->GetOp();
        if (op != ir::OpCode::And && op != ir::OpCode::AndNot && op != ir::OpCode::Or && op != ir::OpCode::Xor) continue;
        const bool branch_only = analysis.HasPseudoUse(producer, ir::OpCode::BranchOnlyFlags);
        if (!branch_only) plan.compound_clears.emplace(clear, producer);
    }
    bool has_flag_tests = false;
    for (auto* inst : analysis.Instructions()) {
        const auto op = inst->GetOp();
        has_flag_tests |= op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags;
        if (!inst->HasValue() || inst->GetUses() != 1) continue;
        const auto uses = analysis.Uses(inst);
        if ((op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags ||
             op == ir::OpCode::TestZero || op == ir::OpCode::TestNotZero) && uses.size() == 1 && uses.front().count == 1)
            plan.sole_users.emplace(inst, uses.front().consumer);
        if (op != ir::OpCode::CondSet && op != ir::OpCode::LocalCondSet && op != ir::OpCode::FCmpCondSet &&
            op != ir::OpCode::TestFlags && op != ir::OpCode::TestNotFlags && op != ir::OpCode::TestZero &&
            op != ir::OpCode::TestNotZero && op != ir::OpCode::And && op != ir::OpCode::Or) continue;
        const auto first = std::find_if(uses.begin(), uses.end(), [&](const auto& use) { return use.consumer->Id() > inst->Id(); });
        if (first == uses.end()) {
            const auto terminal_condition = terminal_conditions.find(inst);
            if (terminal_condition != terminal_conditions.end() && terminal_condition->second &&
                can_defer_local(inst, UINT32_MAX)) plan.local_conditions.insert(inst);
            continue;
        }
        auto* user = first->consumer;
        if ((user->GetOp() == ir::OpCode::Goto || user->GetOp() == ir::OpCode::NotGoto || user->GetOp() == ir::OpCode::Select) &&
            user->GetArg<ir::Value>(0).Def() == inst && can_defer_local(inst, user->Id())) plan.local_conditions.insert(inst);
    }
    if (has_flag_tests) plan.pstate.emplace(BuildPstateFacts(analysis));
    bool later_control = false, last = true;
    bool later_pstate = terminal_pstate;
    for (auto* inst : std::views::reverse(analysis.Instructions())) {
        const auto op = inst->GetOp();
        if ((op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags) && later_pstate)
            plan.later_pstate.insert(inst);
        if (op == ir::OpCode::CondSet || op == ir::OpCode::TestZero || op == ir::OpCode::TestNotZero) {
            BooleanConsumerAction action{nullptr, later_control, last};
            const auto uses = analysis.Uses(inst);
            if (inst->GetUses() == 1 && uses.size() == 1 && uses.front().count == 1) {
                auto* user = uses.front().consumer;
                if (user->GetOp() == ir::OpCode::And || user->GetOp() == ir::OpCode::Or)
                    action.logical_user = user;
            }
            plan.booleans.emplace(inst, action);
        } else if (op == ir::OpCode::VecFCmp) {
            plan.comparisons.emplace(inst, BuildFCmpAction(plan, analysis, inst, retains_nzcv, observes));
        } else if (op == ir::OpCode::FCmpCondSet || op == ir::OpCode::PublishFCmpFlags) {
            auto* producer = inst->GetArg<ir::Value>(0).Def();
            if (producer && producer->GetOp() == ir::OpCode::VecFCmp && !analysis.Contains(producer)) {
                // Compact eligibility is block-local at both ends. Record
                // incoming storage without importing the defining block's
                // raw-NZCV reader certificates across an unproved edge.
                plan.comparisons.try_emplace(producer, ConsumerPlan::FCmpAction{.packed_relation = true});
            }
        } else if (op == ir::OpCode::Sse42Str) {
            auto flags = ir::Flags::None;
            for (const auto& use : analysis.Uses(inst)) {
                if (use.consumer->GetOp() == ir::OpCode::PublishSse42StrFlags &&
                    use.consumer->GetArg<ir::Value>(0).Def() == inst) {
                    flags = use.consumer->GetArg<ir::Flags>(1); break;
                }
            }
            plan.string_flags.emplace(inst, flags);
        }
        later_control |= op == ir::OpCode::Goto || op == ir::OpCode::NotGoto || op == ir::OpCode::BindLabel;
        switch (op) {
            case ir::OpCode::CondSet: case ir::OpCode::CondSelect: case ir::OpCode::LocalCondSet:
            case ir::OpCode::FCmpCondSet: case ir::OpCode::InvertCarry: case ir::OpCode::Adc:
            case ir::OpCode::Sbb: case ir::OpCode::TestFlags: case ir::OpCode::TestNotFlags:
            case ir::OpCode::VecFCmp: case ir::OpCode::PublishFCmpFlags: case ir::OpCode::PublishSse42StrFlags:
                later_pstate = true; break;
            default: break;
        }
        last = false;
    }
    return plan;
}
}
