#include <algorithm>
#include <array>
#include "lower_consumers.h"
#include "lower_flags.h"
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {

namespace {
ir::Value Resolve(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation())
        value = value.Def()->GetArg<ir::Value>(0);
    return value;
}
bool Shares(compile::AllocationView allocation, ir::Value a, ir::Value b, u32 point) {
    if (allocation.PreservedGPRDefinitionAt(a.Id()) ||
        allocation.PreservedGPRDefinitionAt(b.Id())) {
        const auto x =
                allocation.GPRLocationAt(a, point, compile::AllocationView::GPRView::WholeValue);
        const auto y =
                allocation.GPRLocationAt(b, point, compile::AllocationView::GPRView::WholeValue);
        return x.kind == compile::AllocationView::GPR && y.kind == compile::AllocationView::GPR &&
               x.slot == y.slot;
    }
    return allocation.ValueType(a) == compile::AllocationView::GPR &&
           allocation.ValueType(b) == compile::AllocationView::GPR &&
           allocation.ValueGPR(a).id == allocation.ValueGPR(b).id;
}
bool Wrapper(compile::AllocationView allocation,
             const ir::Inst* producer,
             const ir::Inst* wrapper,
             u32 point) {
    if (ir::GetValueSizeByte(producer->ReturnType()) != 4 || !wrapper ||
        wrapper->GetOp() != ir::OpCode::ZeroExtend32To64)
        return false;
    const auto source = Resolve(wrapper->GetArg<ir::Value>(0));
    return source.Def() == producer && Shares(allocation, source, ir::Value{wrapper}, point);
}
bool CapturesParity(const ir::Inst* producer,
                    compile::BlockAnalysisIndex& analysis,
                    const compile::PseudoFlagsPlan& demands) {
    if (demands.SavesParity(producer)) return true;
    // Only funnel OR results can be captured by a later parity OR, possibly
    // through an AND mask. This is demand selection, not fusion discovery.
    if (producer->GetOp() != ir::OpCode::Or) return false;
    for (const auto& use : analysis.PrepareUsesOf(producer)) {
        if (use.consumer->GetOp() == ir::OpCode::Or && demands.SavesParity(use.consumer))
            return true;
        if (use.consumer->GetOp() == ir::OpCode::And)
            for (const auto& next : analysis.PrepareUsesOf(use.consumer))
                if (next.consumer->GetOp() == ir::OpCode::Or && demands.SavesParity(next.consumer))
                    return true;
    }
    return false;
}
}  // namespace

FlagsTokenActions LowerFlagsTokens(compile::BlockAnalysisIndex& analysis,
                                   compile::CompileWorkspace& workspace,
                                   compile::AllocationView allocation,
                                   const FeatureSet& features,
                                   std::span<const ir::Inst* const> prepared_publications,
                                   const compile::PseudoFlagsPlan& demands) {
    const auto nodes = analysis.Instructions();
    ASSERT(nodes.size() <= size_t{UINT16_MAX} + 1);
    // Candidate roles retain the selected home and the decision to use the
    // source of a zero extension. They never leave this exclusive borrow.
    constexpr u16 kHomeMask = 31, kNarrowSource = 32;
    std::optional<compile::LoweringCandidateScope> candidates;
    // Only coalesced publications can retain a result; ordinary blocks avoid
    // constructing the physical suffix indices altogether.
    const auto select = [&](const ir::Inst* inst) {
        if (inst->GetOp() != ir::OpCode::SetHostGPR || inst->GetArg<ir::Imm>(2).Get() != 0 ||
            !allocation.IsHostWriteCoalesced(inst->Id()))
            return;
        const auto home = inst->GetArg<ir::Imm>(1).Get();
        if (!IsFixedGPRHome(home)) return;
        u16 roles = static_cast<u16>(home);
        auto stored = Resolve(inst->GetArg<ir::Value>(0));
        if (!stored.Defined()) return;
        auto* producer = stored.Def();
        if (producer->GetOp() == ir::OpCode::ZeroExtend32To64) {
            auto* source = Resolve(producer->GetArg<ir::Value>(0)).Def();
            // Keep the source candidate even if its point-dependent wrapper
            // tie is rejected below. The extension itself can also publish.
            if (source && ir::GetValueSizeByte(source->ReturnType()) == 4) {
                producer = source;
                roles |= kNarrowSource;
            }
        }
        const auto width = ir::GetValueSizeByte(producer->ReturnType());
        const auto position = analysis.InstructionPosition(inst);
        if ((width != 4 && width != 8) || producer->Id() >= inst->Id() ||
            !analysis.Contains(producer) || !position ||
            !CapturesParity(producer, analysis, demands))
            return;
        if (!candidates) candidates.emplace(workspace);
        candidates->Entries().push_back({static_cast<u16>(*position), roles});
    };
    for (const auto* inst : prepared_publications) select(inst);
    FlagsTokenActions lowered;
    if (!candidates) return lowered;
    struct PhysicalSuffix {
        const ir::Inst* publication{};
        const ir::Inst* definition{};
        u32 previous_publication{};
        u32 previous_definition{};
        u32 last_clobber{};
    };
    std::array<PhysicalSuffix, 31> suffixes{};
    bool preserved = false;
    for (auto* inst : nodes) {
        if (inst->GetOp() == ir::OpCode::SetHostGPR) {
            const auto home = inst->GetArg<ir::Imm>(1).Get();
            if (home < 31) {
                auto& suffix = suffixes[home];
                if (suffix.publication) suffix.previous_publication = suffix.publication->Id();
                suffix.publication = inst;
            }
        }
        const auto clobbers = FixedGPRClobbers(*inst, features);
        for (unsigned home = 0; home < 31; ++home)
            if (clobbers & (1u << home)) suffixes[home].last_clobber = inst->Id();
        if (!inst->HasValue() || inst->IsBitCastOperation()) continue;
        preserved |= allocation.PreservedGPRDefinitionAt(inst->Id()) != nullptr;
        const ir::Value value{inst};
        if (allocation.ValueType(value) == compile::AllocationView::GPR) {
            auto& suffix = suffixes[allocation.ValueGPR(value).id];
            if (suffix.definition) suffix.previous_definition = suffix.definition->Id();
            suffix.definition = inst;
        }
    }
    auto& entries = candidates->Entries();
    const size_t candidate_count = entries.size();
    for (size_t selected = 0; selected < candidate_count; ++selected) {
        // The fixed head holds publication candidates; only the tail can grow
        // for this candidate's capture points. Copy before any reallocation.
        entries.resize(candidate_count);
        const auto candidate = entries[selected];
        const auto* publication = nodes[candidate.position];
        const auto stored = Resolve(publication->GetArg<ir::Value>(0));
        auto* producer = stored.Def();
        if (candidate.roles & kNarrowSource)
            producer = Resolve(producer->GetArg<ir::Value>(0)).Def();
        const auto id = producer->Id();
        const auto home = candidate.roles & kHomeMask;
        const auto& publication_suffix = suffixes[home];
        // The selected write must be the only publication after the producer.
        // Earlier publications do not affect the retained suffix.
        if (publication_suffix.publication != publication ||
            publication_suffix.previous_publication > id || publication_suffix.last_clobber > id)
            continue;
        if (preserved) {
            const auto add_point = [&](const ir::Inst* point) {
                const auto position = analysis.InstructionPosition(point);
                ASSERT(position);
                entries.push_back({static_cast<u16>(*position), 0});
            };
            // A funnel PF consumer can capture its earlier result, directly
            // or through an AND mask. Plan those capture sites explicitly.
            for (const auto& use : analysis.PrepareUsesOf(producer)) {
                if (use.consumer->GetOp() == ir::OpCode::Or) add_point(use.consumer);
                if (use.consumer->GetOp() == ir::OpCode::And)
                    for (const auto& next : analysis.PrepareUsesOf(use.consumer))
                        if (next.consumer->GetOp() == ir::OpCode::Or) add_point(next.consumer);
            }
            if (entries.size() != candidate_count) {
                add_point(producer);
                const auto first = entries.begin() + candidate_count;
                std::ranges::sort(first, entries.end(), {}, &compile::LoweringCandidate::position);
                const auto duplicates = std::ranges::unique(
                        first, entries.end(), {}, &compile::LoweringCandidate::position);
                entries.erase(duplicates.begin(), duplicates.end());
            }
        }
        const size_t point_count = entries.size() - candidate_count;
        for (size_t point_index = 0; point_index < (point_count ? point_count : 1); ++point_index) {
            const u32 point =
                    point_count ? nodes[entries[candidate_count + point_index].position]->Id() : id;
            if (stored.Def() != producer && !Wrapper(allocation, producer, stored.Def(), point))
                continue;
            bool safe = true;
            const ir::Inst* wrapper = nullptr;
            const auto check = [&](const ir::Inst* other) {
                if (!wrapper && Wrapper(allocation, producer, other, point)) {
                    wrapper = other;
                    return;
                }
                if (other != wrapper &&
                    Shares(allocation, ir::Value{producer}, ir::Value{other}, point))
                    safe = false;
            };
            if (preserved) {
                for (auto* node : nodes)
                    if (node->Id() > id && node->HasValue() && !node->IsBitCastOperation())
                        check(node);
            } else if (allocation.ValueType(ir::Value{producer}) == compile::AllocationView::GPR) {
                const auto& suffix = suffixes[allocation.ValueGPR(ir::Value{producer}).id];
                // Without preservation every definition in this home aliases
                // the producer. At most one later definition is permitted,
                // and it must be the existing zero-extension wrapper.
                if (suffix.previous_definition > id) continue;
                if (suffix.definition && suffix.definition->Id() > id) check(suffix.definition);
            }
            if (!safe) continue;
            ASSERT(id < UINT16_MAX && (!preserved || point < UINT16_MAX));
            lowered.retentions.push_back({1u << home,
                                          static_cast<u16>(id),
                                          static_cast<u16>(preserved ? point : UINT16_MAX)});
        }
    }
    std::sort(
            lowered.retentions.begin(), lowered.retentions.end(), [](const auto& a, const auto& b) {
                return std::pair{a.producer, a.point} < std::pair{b.producer, b.point};
            });
    size_t count = 0;
    for (const auto row : lowered.retentions) {
        if (count && lowered.retentions[count - 1].producer == row.producer &&
            lowered.retentions[count - 1].point == row.point)
            lowered.retentions[count - 1].homes |= row.homes;
        else
            lowered.retentions[count++] = row;
    }
    lowered.retentions.resize(count);
    return lowered;
}

namespace {

const ir::Inst* SoleUser(compile::BlockAnalysisIndex& analysis, const ir::Inst* definition) {
    if (!definition || definition->GetUses(false) != 1) return nullptr;
    const auto uses = analysis.PrepareUsesOf(definition);
    return uses.size() == 1 && uses.front().count == 1 ? uses.front().consumer : nullptr;
}

const ir::Inst* OtherOperand(const ir::Inst* combine, const ir::Inst* known) {
    if (!combine || (combine->GetOp() != ir::OpCode::And && combine->GetOp() != ir::OpCode::Or)) {
        return nullptr;
    }
    if (combine->GetArg<ir::Value>(0).Def() == known) {
        const auto right = combine->GetArg<ir::Operand>(1).GetLeft();
        return right.IsValue() ? right.value.Def() : nullptr;
    }
    return combine->GetArg<ir::Value>(0).Def();
}

}  // namespace

CarryActions LowerCarryConditions(const ir::Block* block,
                                  compile::BlockAnalysisIndex& analysis,
                                  const FeatureSet& features,
                                  const ConsumerActions& consumers) {
    CarryActions lowered;
    if (!consumers.valid) return lowered;
    auto& instructions = block->GetInstList();
    for (auto test = instructions.begin(); test != instructions.end(); ++test) {
        if (test->GetOp() != ir::OpCode::TestFlags ||
            test->GetArg<ir::Flags>(0) != ir::Flags::Carry || test->HasPseudoOperations()) {
            continue;
        }
        auto* predicate = SoleUser(analysis, &*test);
        auto* combine = SoleUser(analysis, predicate);
        auto* other = OtherOperand(combine, predicate);
        auto* user = SoleUser(analysis, combine);
        if (!predicate || !combine || !other || other->GetOp() != ir::OpCode::CondSet ||
            SoleUser(analysis, other) != combine ||
            !consumers.Has(combine->Id(), ConsumerFlag::LocalCondition) ||
            predicate->Id() <= test->Id() || other->Id() <= test->Id() ||
            predicate->Id() >= combine->Id() || other->Id() >= combine->Id())
            continue;
        const auto operand = combine->GetArg<ir::Operand>(1);
        if (operand.GetOp() != ir::OperandPlus || !operand.GetRight().Null()) continue;
        const ir::Cond zero_condition = other->GetArg<ir::Cond>(0);
        const bool above = predicate->GetOp() == ir::OpCode::TestZero &&
                           combine->GetOp() == ir::OpCode::And && zero_condition == ir::Cond::NE;
        const bool below_equal = predicate->GetOp() == ir::OpCode::TestNotZero &&
                                 combine->GetOp() == ir::OpCode::Or &&
                                 zero_condition == ir::Cond::EQ;
        if (!above && !below_equal) continue;
        if (user && user->Id() <= combine->Id()) continue;
        const auto end = user ? user->Id() : UINT32_MAX;
        const auto keeps_carrier = [&](const ir::Inst& inst) {
            if (inst.HasPseudoOperations()) return false;
            // These three nodes execute only TickIR/preservation transport
            // after this action records the logical user's local condition.
            if (&inst == predicate || &inst == other || &inst == combine) return true;
            const auto effects = AnalyzeTargetWindowEffects(inst, features, true, std::nullopt);
            return effects.retains_nzcv && !effects.observes_guest;
        };
        bool keeps = true;
        for (auto* inst : analysis.Between(test->Id(), end))
            if (!keeps_carrier(*inst)) {
                keeps = false;
                break;
            }
        if (!keeps) continue;
        ASSERT(test->Id() < UINT16_MAX && combine->Id() < UINT16_MAX);
        auto& action = lowered.actions.emplace_back(
                CarryAction{.instruction = test->Id(),
                            .combine = combine->Id(),
                            .packed_condition = above ? ir::Cond::EQ : ir::Cond::NE});
        auto advance = test;
        if (advance == instructions.begin() || (--advance)->GetOp() != ir::OpCode::AdvancePC) {
            continue;
        }
        auto invert = advance;
        if (invert == instructions.begin() || (--invert)->GetOp() != ir::OpCode::InvertCarry) {
            continue;
        }
        auto save = invert;
        if (save == instructions.begin() || (--save)->GetOp() != ir::OpCode::SaveFlags ||
            !True(save->GetArg<ir::Flags>(1) & ir::Flags::Carry)) {
            continue;
        }
        auto* producer = save->GetArg<ir::Value>(0).Def();
        if (!producer ||
            (producer->GetOp() != ir::OpCode::Sub && producer->GetOp() != ir::OpCode::Sbb)) {
            continue;
        }
        // SaveFlags is a pseudo attached to the producer, but a later helper
        // or flags producer can consume/overwrite its pending host NZCV before
        // the publication node is reached. Freeze only a preserved local window.
        bool preserved = true;
        const auto keeps_producer = [&](const ir::Inst& inst) {
            const auto effects = AnalyzeTargetWindowEffects(inst, features, true, std::nullopt);
            return !inst.HasPseudoOperations() && effects.retains_nzcv && !effects.observes_guest;
        };
        if (!analysis.Contains(producer) || producer->Id() >= save->Id()) continue;
        for (auto* inst : analysis.Between(producer->Id(), save->Id()))
            if (!keeps_producer(*inst)) {
                preserved = false;
                break;
            }
        if (!preserved) continue;
        ASSERT(invert->Id() < UINT16_MAX);
        action.invert = invert->Id();
        action.raw_condition = above ? ir::Cond::HI : ir::Cond::LS;
    }
    return lowered;
}

const CarryAction* CarryActions::Find(u32 id) const {
    const auto found = std::find_if(actions.begin(), actions.end(), [id](const auto& row) {
        return row.instruction == id;
    });
    return found == actions.end() ? nullptr : &*found;
}

namespace {
bool PstateClobbers(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::TestFlags:
        case ir::OpCode::TestNotFlags:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::VecFCmp:
        case ir::OpCode::PublishFCmpFlags:
        case ir::OpCode::PublishSse42StrFlags:
        case ir::OpCode::X87Op:
        case ir::OpCode::VecShiftLeft:
        case ir::OpCode::VecShiftRight:
        case ir::OpCode::VecShiftRightArithmetic:
            return true;
        default:
            return false;
    }
}
}  // namespace
PstateEligibility PreparePstateEligibility(const compile::BlockAnalysisIndex& analysis) {
    PstateEligibility facts;
    ir::Flags simple{};
    bool saved = false, clobbered = false, inverted = false, add_family = false;
    constexpr auto simple_mask = ir::Flags::Zero | ir::Flags::Negate | ir::Flags::Overflow;
    for (auto* inst : analysis.Instructions()) {
        const auto op = inst->GetOp();
        if (op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags)
            facts.tests.push_back(
                    {inst,
                     static_cast<u8>(simple | ((inverted || add_family) ? ir::Flags::Carry
                                                                        : ir::Flags::None))});
        if (op == ir::OpCode::AdvancePC && !facts.first_advance) facts.first_advance = inst->Id();
        if (op == ir::OpCode::SaveFlags || op == ir::OpCode::BranchOnlyFlags) {
            const auto flags = inst->GetArg<ir::Flags>(1);
            simple = flags & simple_mask;
            if (True(flags & ir::Flags::Carry)) {
                saved = true;
                clobbered = false;
                inverted = false;
                auto* producer = inst->GetArg<ir::Value>(0).Def();
                add_family = producer && (producer->GetOp() == ir::OpCode::Add ||
                                          producer->GetOp() == ir::OpCode::Adc);
            }
        } else if (op == ir::OpCode::ClearFlags) {
            const auto cleared = inst->GetArg<ir::Flags>(0);
            simple &= ~cleared;
            if (True(cleared & ir::Flags::Carry)) {
                saved = false;
                inverted = false;
                add_family = false;
                clobbered = true;
            }
        } else if (op == ir::OpCode::SetOverflow)
            simple &= ~ir::Flags::Overflow;
        else if (op == ir::OpCode::InvertCarry) {
            inverted = saved && !clobbered;
            add_family = false;
        } else if (op == ir::OpCode::SetCarry) {
            saved = false;
            inverted = false;
            add_family = false;
            clobbered = true;
        } else if (PstateClobbers(op)) {
            simple = ir::Flags::None;
            clobbered = true;
            inverted = false;
            add_family = false;
        }
    }
    return facts;
}
ir::Flags PstateEligibility::At(const ir::Inst* test) const {
    if (!test) return ir::Flags::None;
    const auto found =
            std::lower_bound(tests.begin(), tests.end(), test->Id(), [](const auto& row, u32 key) {
                return row.instruction->Id() < key;
            });
    return found != tests.end() && found->instruction == test ? static_cast<ir::Flags>(found->mask)
                                                              : ir::Flags::None;
}

}  // namespace swift::runtime::backend::arm64
