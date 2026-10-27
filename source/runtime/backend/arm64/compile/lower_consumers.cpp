#include <ranges>
#include "lower_consumers.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/common/variant_util.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/consumer_roles.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {

namespace {
struct ConsumerBuilder final {
    static constexpr u32 kRowMask = 0x00ffffff;
    static constexpr u32 kTerminal = 1u << 24, kTerminalNZCV = 1u << 25;
    const compile::BlockAnalysisIndex& analysis;
    ConsumerActions lowered;
    compile::InstructionRowScope rows;
    compile::LoweringCandidateScope candidates;
    void SeedCandidates(bool audit) {
        const auto nodes = analysis.Instructions();
        ASSERT(nodes.size() <= size_t{UINT16_MAX} + 1);
        for (size_t position = 0; position < nodes.size(); ++position) {
            auto roles = compile::ConsumerRoles(nodes[position]->GetOp());
            if (!audit)
                roles = static_cast<compile::ConsumerRole>(
                        static_cast<u8>(roles) & ~static_cast<u8>(compile::ConsumerRole::AluAudit));
            if (!analysis.Summary().HasSingleUseValue() || !nodes[position]->HasValue() ||
                nodes[position]->GetUses() != 1)
                roles = static_cast<compile::ConsumerRole>(
                        static_cast<u8>(roles) &
                        ~static_cast<u8>(compile::ConsumerRole::LocalCondition));
            if (roles != compile::ConsumerRole::None)
                candidates.Entries().push_back(
                        {static_cast<u16>(position), static_cast<u16>(roles)});
        }
    }
    u32& State(const ir::Inst* inst) {
        const auto position = analysis.InstructionPosition(inst);
        ASSERT(position);
        auto& state = rows[*position];
        if (state == UINT32_MAX) state = kRowMask;
        return state;
    }
    ConsumerAction& Touch(u16 id) { return Touch(analysis.FindInstruction(id)); }
    ConsumerAction& Touch(const ir::Inst* inst) {
        auto& state = State(inst);
        if ((state & kRowMask) == kRowMask) {
            ASSERT(lowered.actions.size() < kRowMask);
            state = (state & ~kRowMask) | static_cast<u32>(lowered.actions.size());
            lowered.actions.push_back(ConsumerAction{inst->Id()});
        }
        return lowered.actions[state & kRowMask];
    }
    const ConsumerAction* Find(const ir::Inst* inst) const {
        const auto position = analysis.InstructionPosition(inst);
        if (!position) return nullptr;
        const auto row = rows.Find(*position);
        return (row & kRowMask) == kRowMask ? nullptr : &lowered.actions[row & kRowMask];
    }
    void ObserveTerminalCondition(const ir::Inst* inst, bool retains_nzcv) {
        // Only local producers are queried by this block's lowering. Multiple
        // terminal occurrences must all preserve the earlier NZCV value.
        if (!analysis.Contains(inst)) return;
        auto& state = State(inst);
        if (!(state & kTerminal)) state |= kTerminal | kTerminalNZCV;
        if (!retains_nzcv) state &= ~kTerminalNZCV;
    }
    bool TerminalConditionCanDefer(const ir::Inst* inst) const {
        const auto position = analysis.InstructionPosition(inst);
        if (!position) return false;
        const auto state = rows.Find(*position);
        return state != UINT32_MAX &&
               (state & (kTerminal | kTerminalNZCV)) == (kTerminal | kTerminalNZCV);
    }
    ConsumerActions Finish() {
        std::sort(lowered.actions.begin(), lowered.actions.end(), [](const auto& a, const auto& b) {
            return a.instruction < b.instruction;
        });
        return std::move(lowered);
    }
};
struct FCmpDecision {
    bool compact_carrier{}, packed_relation{};
    const ir::Inst* raw_condition{};
};
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

bool DataIsDef(const ir::Operand::Type& data, const ir::Inst* def) {
    return data.IsValue() && data.value.Def() == def;
}

bool InstRefsDef(const ir::Inst* user, const ir::Inst* def) {
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

bool IsAddressUse(const ir::Inst* user, const ir::Inst* def) {
    using O = ir::OpCode;
    const auto op = user->GetOp();
    if (op == O::GetOperand || op == O::LoadMemory || op == O::LoadMemoryTSO) {
        return true;
    }
    if (op == O::StoreMemory || op == O::StoreMemoryTSO) {
        return user->GetArg<ir::Value>(1).Def() != def;
    }
    if (op == O::AtomicExchange || op == O::AtomicFetchAdd) {
        return user->GetArg<ir::Value>(0).Def() == def && user->GetArg<ir::Value>(1).Def() != def;
    }
    if (op == O::AtomicRMW) {
        return user->GetArg<ir::Value>(1).Def() == def;
    }
    return false;
}

AluSplitFacts BuildAluSplitFacts(const compile::BlockAnalysisIndex& analysis,
                                 const ir::Inst* inst) {
    AluSplitFacts out;
    out.value_uses = inst->GetUses(true);
    std::array<const ir::Inst*, 32> family{};
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
        const ir::Inst* which = nullptr;
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

bool BuildPairResults(ConsumerBuilder& builder, compile::BlockAnalysisIndex& analysis) {
    const auto nodes = analysis.Instructions();
    for (const auto candidate : builder.candidates.Entries()) {
        if (!compile::HasRole(candidate.roles, compile::ConsumerRole::PairProducer)) continue;
        const auto* producer = nodes[candidate.position];
        const auto op = producer->GetOp();
        if (op != ir::OpCode::Div128 && op != ir::OpCode::Cpuid) continue;
        if (producer->ReturnType() != ir::ValueType::U64) return false;
        const auto secondary_opcode =
                op == ir::OpCode::Div128 ? ir::OpCode::Div128Remainder : ir::OpCode::CpuidUpper;
        const ir::Inst* secondary_result{};
        u32 local_pseudo_uses = 0;
        for (const auto& use : analysis.PrepareUsesOf(producer)) {
            auto* pseudo = use.consumer;
            if (!pseudo->IsPseudoOperation()) continue;
            local_pseudo_uses += use.count;
            // A pair secondary must consume its producer exactly once,
            // as operand 0 (a malformed double use is rejected here,
            // replacing the old pseudo-chain cycle guard).
            if (use.count != 1 || !analysis.Contains(pseudo) || !pseudo->ArgAt(0).IsValue() ||
                pseudo->GetArg<ir::Value>(0).Def() != producer || pseudo->Id() <= producer->Id())
                return false;
            const auto secondary = pseudo->GetOp();
            if (secondary != ir::OpCode::Div128Remainder && secondary != ir::OpCode::CpuidUpper)
                continue;
            if (secondary != secondary_opcode || secondary_result ||
                pseudo->ReturnType() != ir::ValueType::U64 ||
                pseudo->GetArg<ir::Value>(0).Type() != ir::ValueType::U64)
                return false;
            secondary_result = pseudo;
        }
        // A pseudo use from another block is invisible to this
        // block's index, but the producer's use counters still see
        // it; any unaccounted pseudo use is foreign ownership.
        if (producer->GetUses(false) - producer->GetUses() != local_pseudo_uses) return false;
        auto& action = builder.Touch(producer);
        action.Mark(ConsumerFlag::Pair);
        if (secondary_result) action.related = secondary_result->Id();
    }
    // A local secondary must also be reachable from its admitted local owner.
    for (const auto candidate : builder.candidates.Entries()) {
        if (!compile::HasRole(candidate.roles, compile::ConsumerRole::PairSecondary)) continue;
        const auto* node = nodes[candidate.position];
        const auto op = node->GetOp();
        if (op != ir::OpCode::Div128Remainder && op != ir::OpCode::CpuidUpper) continue;
        if (!node->ArgAt(0).IsValue()) return false;
        const auto* owner = builder.Find(node->GetArg<ir::Value>(0).Def());
        if (!owner || !owner->Has(ConsumerFlag::Pair) || owner->related != node->Id()) return false;
    }
    return true;
}

FCmpDecision BuildFCmpAction(ConsumerBuilder& builder,
                             compile::BlockAnalysisIndex& analysis,
                             const ir::Inst* producer,
                             const std::function<bool(const ir::Inst&)>& retains_nzcv,
                             const std::function<bool(const ir::Inst&)>& observes) {
    FCmpDecision action;
    const auto uses = analysis.PrepareUsesOf(producer);
    const bool compact = producer->GetArg<ir::Imm>(3).Get() != 0;
    if (uses.empty()) {
        // A named relation crossing a block edge cannot borrow this block's
        // host NZCV. Its receiving plan consumes the full SSA relation.
        action.packed_relation = compact && producer->GetUses(false) != 0;
        return action;
    }
    const bool sole_condition = producer->GetUses() == 1 && uses.size() == 1 &&
                                uses.front().count == 1 &&
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
    const auto first = std::upper_bound(
            nodes.begin(), nodes.end(), producer->Id(), [](u32 id, const ir::Inst* node) {
                return id < node->Id();
            });
    bool raw_safe = true, compact_safe = true, published = false;
    unsigned readers = 0;
    StackVector<const ir::Inst*, 2> raw_readers;
    u32 inversion_count{};
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
                if (sole_condition && raw_safe && !published)
                    action.raw_condition = node;
                else if (raw_safe)
                    raw_readers.push_back(node);
                const auto cond = node->GetArg<ir::Cond>(1);
                const bool supported = cond == ir::Cond::LT || cond == ir::Cond::GE ||
                                       cond == ir::Cond::GT || cond == ir::Cond::LE ||
                                       cond == ir::Cond::VS || cond == ir::Cond::VC;
                if (compact && node != action.raw_condition &&
                    (!published || !compact_safe || !supported))
                    action.packed_relation = true;
                carrier &= published && compact_safe && readers == 1;
                if (compact && inversion_count) {
                    auto& reader = builder.Touch(node);
                    reader.related = producer->Id();
                    if (inversion_count & 1) reader.Mark(ConsumerFlag::OddCarry);
                }
            }
            continue;  // All relation-reader encodings preserve host NZCV.
        }
        if (op == ir::OpCode::InvertCarry) {
            if (compact) ++inversion_count;
            raw_safe = false;
            if (!published) compact_safe = false;
            continue;
        }
        if (analysis.PrepareHasPseudoUse(node) || !retains_nzcv(*node) ||
            (!published && observes(*node))) {
            raw_safe = false;
            compact_safe = false;
        }
    }
    action.compact_carrier = compact && !action.packed_relation && carrier && published &&
                             uses.size() == producer->GetUses();
    if (!compact || action.packed_relation) {
        for (const auto* reader : raw_readers) builder.Touch(reader).Mark(ConsumerFlag::RawReader);
    }
    if (action.raw_condition) builder.Touch(action.raw_condition).Mark(ConsumerFlag::RawReader);
    return action;
}
}  // namespace
ConsumerActions LowerConsumers(compile::BlockAnalysisIndex& analysis,
                               compile::CompileWorkspace& workspace,
                               const std::function<bool(const ir::Inst&)>& retains_nzcv,
                               const std::function<bool(const ir::Inst&)>& observes,
                               const ir::Terminal* terminal,
                               bool alu_split_audit) {
    ConsumerBuilder builder{analysis,
                            {},
                            {workspace, analysis.Instructions().size()},
                            compile::LoweringCandidateScope{workspace}};
    builder.SeedCandidates(alu_split_audit);
    auto& plan = builder.lowered;
    plan.valid = BuildPairResults(builder, analysis);
    if (!plan.valid) return builder.Finish();
    if (alu_split_audit)
        for (const auto candidate : builder.candidates.Entries()) {
            if (!compile::HasRole(candidate.roles, compile::ConsumerRole::AluAudit)) continue;
            const auto* inst = analysis.Instructions()[candidate.position];
            if (IsAluSplitOpcode(inst->GetOp()))
                plan.audit.push_back({inst->Id(), BuildAluSplitFacts(analysis, inst)});
        }
    bool terminal_pstate = false;
    const auto visit = [&](auto&& self, const ir::Terminal& node, bool pstate_reloaded) -> void {
        VisitVariant<void>(node, [&](const auto& edge) {
            using T = std::decay_t<decltype(edge)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                if (edge.cond.Def()) {
                    builder.ObserveTerminalCondition(edge.cond.Def(), !pstate_reloaded);
                }
                self(self, edge.then_, pstate_reloaded);
                self(self, edge.else_, pstate_reloaded);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                terminal_pstate = true;
                // Guest-condition evaluation may reload canonical NZCV. A
                // descendant raw predicate must retain its earlier SSA value.
                self(self, edge.then_, true);
                self(self, edge.else_, true);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>)
                self(self, edge.else_, pstate_reloaded);
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>)
                for (const auto& arm : edge.cases) self(self, arm.then, pstate_reloaded);
        });
    };
    if (terminal) visit(visit, *terminal, false);
    const auto nodes = analysis.Instructions();
    const auto can_defer_local = [&](const ir::Inst* condition, u32 before) {
        // These readers capture host NZCV at their own IR point. Keeping only
        // a condition code must prove the second window through the user too.
        if (condition->GetOp() != ir::OpCode::FCmpCondSet &&
            condition->GetOp() != ir::OpCode::LocalCondSet)
            return true;
        for (auto* node : analysis.Between(condition->Id(), before))
            if (analysis.PrepareHasPseudoUse(node) || !retains_nzcv(*node) || observes(*node))
                return false;
        return true;
    };
    for (const auto candidate : builder.candidates.Entries()) {
        if (!compile::HasRole(candidate.roles, compile::ConsumerRole::CompoundClear)) continue;
        const size_t i = candidate.position;
        if (!i || i + 1 >= nodes.size()) continue;
        auto* clear = nodes[i];
        if (clear->GetOp() != ir::OpCode::ClearFlags ||
            clear->GetArg<ir::Flags>(0) != (ir::Flags::CV | ir::Flags::AuxiliaryCarry))
            continue;
        auto* producer = nodes[i - 1];
        auto* save = nodes[i + 1];
        if (save->GetOp() != ir::OpCode::SaveFlags ||
            save->GetArg<ir::Value>(0).Def() != producer ||
            save->GetArg<ir::Flags>(1) != (ir::Flags::NZ | ir::Flags::Parity))
            continue;
        const auto op = producer->GetOp();
        if (op != ir::OpCode::And && op != ir::OpCode::AndNot && op != ir::OpCode::Or &&
            op != ir::OpCode::Xor)
            continue;
        const bool branch_only =
                analysis.PrepareHasPseudoUse(producer, ir::OpCode::BranchOnlyFlags);
        if (!branch_only) {
            auto& row = builder.Touch(clear);
            row.Mark(ConsumerFlag::CompoundClear);
            row.related = producer->Id();
        }
    }
    const bool has_flag_tests =
            std::ranges::any_of(builder.candidates.Entries(), [](const auto candidate) {
                return compile::HasRole(candidate.roles, compile::ConsumerRole::PstateTest);
            });
    for (const auto candidate : builder.candidates.Entries()) {
        if (!compile::HasRole(candidate.roles, compile::ConsumerRole::LocalCondition)) continue;
        const auto* inst = nodes[candidate.position];
        const auto op = inst->GetOp();
        if (!inst->HasValue() || inst->GetUses() != 1) continue;
        const auto uses = analysis.PrepareUsesOf(inst);
        if (op != ir::OpCode::CondSet && op != ir::OpCode::LocalCondSet &&
            op != ir::OpCode::FCmpCondSet && op != ir::OpCode::TestFlags &&
            op != ir::OpCode::TestNotFlags && op != ir::OpCode::TestZero &&
            op != ir::OpCode::TestNotZero && op != ir::OpCode::And && op != ir::OpCode::Or)
            continue;
        const auto first = std::find_if(uses.begin(), uses.end(), [&](const auto& use) {
            return use.consumer->Id() > inst->Id();
        });
        if (first == uses.end()) {
            if (builder.TerminalConditionCanDefer(inst) && can_defer_local(inst, UINT32_MAX))
                builder.Touch(inst).Mark(ConsumerFlag::LocalCondition);
            continue;
        }
        auto* user = first->consumer;
        if ((user->GetOp() == ir::OpCode::Goto || user->GetOp() == ir::OpCode::NotGoto ||
             user->GetOp() == ir::OpCode::Select) &&
            user->GetArg<ir::Value>(0).Def() == inst && can_defer_local(inst, user->Id()))
            builder.Touch(inst).Mark(ConsumerFlag::LocalCondition);
    }
    if (has_flag_tests) {
        plan.pstate.emplace(PreparePstateEligibility(analysis));
        for (const auto& test : plan.pstate->tests)
            builder.Touch(test.instruction).pstate_mask = test.mask;
    }
    bool later_control = false;
    bool later_pstate = terminal_pstate;
    for (const auto candidate : std::views::reverse(builder.candidates.Entries())) {
        if (!compile::HasRole(candidate.roles, compile::ConsumerRole::ReverseEvent)) continue;
        const auto* inst = nodes[candidate.position];
        const bool last = size_t{candidate.position} + 1 == nodes.size();
        const auto op = inst->GetOp();
        if ((op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags) && later_pstate)
            builder.Touch(inst).Mark(ConsumerFlag::LaterPstate);
        if (op == ir::OpCode::CondSet || op == ir::OpCode::TestZero ||
            op == ir::OpCode::TestNotZero) {
            auto& action = builder.Touch(inst);
            action.Mark(ConsumerFlag::Boolean);
            if (later_control) action.Mark(ConsumerFlag::LaterControl);
            if (last) action.Mark(ConsumerFlag::Last);
            if (inst->GetUses() == 1) {
                const auto uses = analysis.PrepareUsesOf(inst);
                if (uses.size() == 1 && uses.front().count == 1) {
                    auto* user = uses.front().consumer;
                    if (user->GetOp() == ir::OpCode::And || user->GetOp() == ir::OpCode::Or)
                        action.related = user->Id();
                }
            }
        } else if (op == ir::OpCode::VecFCmp) {
            const auto decision = BuildFCmpAction(builder, analysis, inst, retains_nzcv, observes);
            auto& row = builder.Touch(inst);
            row.Mark(ConsumerFlag::FCmp);
            if (decision.compact_carrier) row.Mark(ConsumerFlag::CompactCarrier);
            if (decision.packed_relation) row.Mark(ConsumerFlag::PackedRelation);
            if (decision.raw_condition) row.Mark(ConsumerFlag::RawSingle);
        } else if (op == ir::OpCode::FCmpCondSet || op == ir::OpCode::PublishFCmpFlags) {
            auto* producer = inst->GetArg<ir::Value>(0).Def();
            if (producer && producer->GetOp() == ir::OpCode::VecFCmp &&
                !analysis.Contains(producer)) {
                // Compact eligibility is block-local at both ends. Record
                // incoming storage without importing the defining block's
                // raw-NZCV reader certificates across an unproved edge.
                builder.Touch(inst).Mark(ConsumerFlag::IncomingPacked);
            }
        } else if (op == ir::OpCode::Sse42Str) {
            auto flags = ir::Flags::None;
            for (const auto& use : analysis.PrepareUsesOf(inst)) {
                if (use.consumer->GetOp() == ir::OpCode::PublishSse42StrFlags &&
                    use.consumer->GetArg<ir::Value>(0).Def() == inst) {
                    flags = use.consumer->GetArg<ir::Flags>(1);
                    break;
                }
            }
            ASSERT(static_cast<u64>(flags) <= UINT8_MAX);
            auto& row = builder.Touch(inst);
            row.Mark(ConsumerFlag::String);
            row.auxiliary = static_cast<u8>(flags);
        }
        later_control |=
                op == ir::OpCode::Goto || op == ir::OpCode::NotGoto || op == ir::OpCode::BindLabel;
        switch (op) {
            case ir::OpCode::CondSet:
            case ir::OpCode::CondSelect:
            case ir::OpCode::LocalCondSet:
            case ir::OpCode::FCmpCondSet:
            case ir::OpCode::InvertCarry:
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
            case ir::OpCode::VecFCmp:
            case ir::OpCode::PublishFCmpFlags:
            case ir::OpCode::PublishSse42StrFlags:
                later_pstate = true;
                break;
            default:
                break;
        }
    }
    return builder.Finish();
}

const ConsumerAction* ConsumerActions::Find(u32 id) const {
    const auto at =
            std::lower_bound(actions.begin(), actions.end(), id, [](const auto& row, u32 key) {
                return row.instruction < key;
            });
    return at != actions.end() && at->instruction == id ? &*at : nullptr;
}

const AluSplitFacts* ConsumerActions::Audit(u32 id) const {
    const auto at = std::lower_bound(audit.begin(), audit.end(), id, [](const auto& row, u32 key) {
        return row.instruction < key;
    });
    return at != audit.end() && at->instruction == id ? &at->facts : nullptr;
}

void ConsumerActions::AddIncomingZeros(compile::BlockAnalysisIndex& analysis,
                                       std::span<const ir::Inst* const> values) {
    for (const auto* value : values)
        for (const auto& use : analysis.PrepareUsesOf(value)) {
            const auto* user = use.consumer;
            const auto op = user->GetOp();
            const bool first = op == ir::OpCode::SetHostFPR || op == ir::OpCode::ZeroExtend32 ||
                               op == ir::OpCode::ZeroExtend32To64;
            ASSERT(first || op == ir::OpCode::StoreMemory || op == ir::OpCode::StoreUniform);
            const u8 slot = first ? 0 : 1;
            ASSERT(user->GetArg<ir::Value>(slot).Def() == value);
            ConsumerAction row{user->Id()};
            row.Mark(ConsumerFlag::IncomingZero);
            row.auxiliary = slot;
            actions.push_back(row);
        }
    std::sort(actions.begin(), actions.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    size_t count = 0;
    for (const auto row : actions) {
        if (count && actions[count - 1].instruction == row.instruction) {
            auto& previous = actions[count - 1];
            ASSERT(previous.related == UINT16_MAX || row.related == UINT16_MAX ||
                   previous.related == row.related);
            ASSERT(!((previous.Has(ConsumerFlag::String) && row.Has(ConsumerFlag::IncomingZero)) ||
                     (row.Has(ConsumerFlag::String) && previous.Has(ConsumerFlag::IncomingZero))));
            if (row.related != UINT16_MAX) previous.related = row.related;
            if (row.Has(ConsumerFlag::IncomingZero) || row.Has(ConsumerFlag::String))
                previous.auxiliary = row.auxiliary;
            previous.properties |= row.properties;
            previous.pstate_mask |= row.pstate_mask;
        } else
            actions[count++] = row;
    }
    actions.resize(count);
}
}  // namespace swift::runtime::backend::arm64
