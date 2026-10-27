#include <array>
#include "lower_integer_flags.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/target.h"
#include "runtime/compile/analysis/effects.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {

namespace {

compile::PseudoFlagsDemand PreparationDemand(const ir::Inst* inst,
                                             const compile::BlockAnalysisIndex& owner,
                                             compile::PseudoFlagsView base,
                                             const IntegerBranchAction* branch) {
    if (!owner.Contains(inst)) return {};
    return branch && branch->producer == inst->Id()
                   ? compile::PseudoFlagsDemand{branch->Required(), ir::Flags::None, true}
                   : base.For(inst);
}

struct CompareSelection {
    IntegerFlagsAction action;
    const ir::Inst* immediate_load{};
};
static_assert(sizeof(CompareSelection) == 16);

std::optional<std::array<ir::Value, 2>> BinaryValues(const ir::Inst* inst) {
    const auto operand = inst->GetArg<ir::Operand>(1);
    if (!operand.GetRight().Null()) return std::nullopt;
    const auto right = operand.GetLeft();
    if (!right.IsValue()) {
        return std::nullopt;
    }
    return std::array{inst->GetArg<ir::Value>(0), right.value};
}

bool IsNarrowPublication(const ir::Inst& consumer, const ir::Inst* definition) {
    if (consumer.GetOp() != ir::OpCode::SetHostGPR &&
        consumer.GetOp() != ir::OpCode::StoreUniform) {
        return false;
    }
    for (const auto value : consumer.GetValues()) {
        if (value.Def() == definition) {
            return ir::GetValueSizeByte(value.Type()) <= sizeof(u16);
        }
    }
    return false;
}

bool IsZeroWrapper(ir::OpCode op) {
    return op == ir::OpCode::ZeroExtend32 || op == ir::OpCode::ZeroExtend32To64 ||
           op == ir::OpCode::BitCast;
}

const ir::Inst* NextZeroInput(const ir::Inst* inst) {
    return IsZeroWrapper(inst->GetOp()) ? inst->GetArg<ir::Value>(0).Def() : nullptr;
}

bool IsZeroInput(ir::Value value) {
    auto* inst = value.Def();
    while (inst && IsZeroWrapper(inst->GetOp())) inst = NextZeroInput(inst);
    return inst && inst->GetOp() == ir::OpCode::LoadImm && inst->GetArg<ir::Imm>(0).Get() == 0;
}

ir::Flags ExplicitFlagRead(const ir::Inst& inst) {
    // This target window must also retain a branch-only producer until its
    // raw PSTATE consumer. It is not an architectural flags read.
    if (inst.GetOp() == ir::OpCode::BranchOnlyFlags) return inst.GetArg<ir::Flags>(1);
    return compile::InstructionFlagAccess(inst).read;
}

bool IsFaultBoundary(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CheckMemoryAlignment:
            return true;
        default:
            return false;
    }
}

bool IsNarrowZeroExtended(ir::Value value, u32 width) {
    if (!value.Def() || (width != sizeof(u8) && width != sizeof(u16))) {
        return false;
    }
    auto* inst = value.Def();
    if (inst->GetOp() == ir::OpCode::LoadMemory || inst->GetOp() == ir::OpCode::LoadMemoryTSO ||
        inst->GetOp() == ir::OpCode::LoadUniform || inst->GetOp() == ir::OpCode::GetHostGPR) {
        return ir::GetValueSizeByte(inst->ReturnType()) == width;
    }
    if (inst->GetOp() == ir::OpCode::ZeroExtend32 ||
        inst->GetOp() == ir::OpCode::ZeroExtend32To64 || inst->GetOp() == ir::OpCode::BitCast) {
        return IsNarrowZeroExtended(inst->GetArg<ir::Value>(0), width);
    }
    return inst->GetOp() == ir::OpCode::BitExtract &&
           ir::GetValueSizeByte(inst->ReturnType()) == width &&
           inst->GetArg<ir::Imm>(1).Get() == 0 && inst->GetArg<ir::Imm>(2).Get() == width * 8;
}

}  // namespace

namespace {
class IntegerFlagsBuilder {
public:
    IntegerFlagsBuilder(const compile::BlockAnalysisIndex& a,
                        compile::CompileWorkspace& workspace,
                        const IntegerFlagsInputs& i)
            : analysis(a), workspace(workspace), inputs(i) {
        if (inputs.pstate)
            pstate = inputs.pstate;
        else {
            owned_pstate.emplace(PreparePstateEligibility(analysis));
            pstate = &*owned_pstate;
        }
    }
    IntegerFlagsActions Build();

private:
    compile::PseudoFlagsDemand Demand(const ir::Inst* inst) const {
        return PreparationDemand(
                inst, inputs.demand_owner, inputs.demands, inputs.selected_branch);
    }
    bool Covered(const ir::Inst* inst) const {
        if (inst->Id() < inputs.earlier_coverage.size() &&
            inputs.earlier_coverage.test(inst->Id()))
            return true;
        const auto position = analysis.InstructionPosition(inst);
        const auto row = position ? Row(*position) : UINT32_MAX;
        return row != UINT32_MAX && (row & kCovered);
    }
    enum class DeadUse : u32 { Visiting = 1, Accepted, Rejected };
    static constexpr u32 kCovered = 1, kPendingDead = 8, kQueryShift = 4;
    u32 Row(size_t position) const { return rows ? rows->Find(position) : UINT32_MAX; }
    u32& WriteRow(size_t position) {
        if (!rows) rows.emplace(workspace, analysis.Instructions().size());
        return (*rows)[position];
    }
    void RecordProof(size_t position, DeadUse state) {
        const auto previous = Row(position);
        const u32 retained = previous == UINT32_MAX ? 0 : previous & kCovered;
        WriteRow(position) = (query << kQueryShift) | (static_cast<u32>(state) << 1) | retained;
    }
    void Cover(const ir::Inst* inst) {
        const auto position = analysis.InstructionPosition(inst);
        ASSERT(position);
        const auto previous = Row(*position);
        if (previous != UINT32_MAX && (previous & kCovered)) return;
        WriteRow(*position) = previous == UINT32_MAX ? kCovered : previous | kCovered;
        plan.covered.push_back(inst->Id());
    }
    bool ProveDeadUses(const ir::Inst* producer, const ir::Inst* carry_add);
    std::optional<IntegerFlagsAction> SelectCarry(const ir::Inst* inst);
    std::optional<CompareSelection> MatchCompare(const ir::Inst* inst);
    const compile::BlockAnalysisIndex& analysis;
    compile::CompileWorkspace& workspace;
    const IntegerFlagsInputs& inputs;
    IntegerFlagsActions plan;
    // Slot keys are block roster positions, never unit-wide numeric IDs.
    // The query stamp belongs only to this matching call: previous selections
    // can change coverage, so their dead-use answers cannot be reused.
    std::optional<compile::InstructionRowScope> rows;
    u32 query{}, latest_carry_test{};
    std::optional<PstateEligibility> owned_pstate;
    const PstateEligibility* pstate{};
};
std::optional<IntegerFlagsAction> IntegerFlagsBuilder::SelectCarry(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::Add ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u32) || !Demand(inst).Null()) {
        return std::nullopt;
    }

    const auto final_values = BinaryValues(inst);
    if (!final_values) {
        return std::nullopt;
    }
    const ir::Inst* carry_add{};
    ir::Value value{};
    for (const auto candidate : *final_values) {
        if (candidate.Def() && candidate.Def()->GetOp() == ir::OpCode::Add) {
            carry_add = candidate.Def();
        } else {
            value = candidate;
        }
    }
    if (!carry_add || !analysis.Contains(carry_add) || !value.Def() || carry_add->GetUses() != 1 ||
        carry_add->Id() + 1 != inst->Id() || !Demand(carry_add).Null()) {
        return std::nullopt;
    }

    const auto carry_values = BinaryValues(carry_add);
    if (!carry_values) {
        return std::nullopt;
    }
    const ir::Inst* carry_test{};
    ir::Value zero{};
    for (const auto candidate : *carry_values) {
        if (candidate.Def() && candidate.Def()->GetOp() == ir::OpCode::TestFlags) {
            carry_test = candidate.Def();
        } else {
            zero = candidate;
        }
    }
    if (!carry_test || !analysis.Contains(carry_test) || carry_test->GetUses() != 1 ||
        carry_test->GetArg<ir::Flags>(0) != ir::Flags::Carry ||
        carry_test->Id() + 1 != carry_add->Id() ||
        False(pstate->At(carry_test) & ir::Flags::Carry) || !IsZeroInput(zero)) {
        return std::nullopt;
    }
    // Once AdvancePC commits a subtraction, the canonical carrier keeps
    // ARM's native C polarity while the fusion consumes live host C. Keep the
    // CFINV fusion within one guest boundary; ordinary narrow lowering handles
    // the cross-boundary case through the canonical carry value.
    if (inputs.canonical_carry && pstate->first_advance &&
        *pstate->first_advance < carry_test->Id())
        return std::nullopt;

    bool narrow_publication = false;
    for (const auto& use : analysis.Uses(inst)) {
        if (!IsNarrowPublication(*use.consumer, inst)) return std::nullopt;
        narrow_publication = true;
    }
    if (!narrow_publication) return std::nullopt;

    // Compute every root before committing any of this selection's coverage.
    // Early commit would let a later root use a proof unavailable to the old
    // candidate, particularly for shared chains and observable aliases.
    ++query;
    ASSERT(query < (UINT32_MAX >> kQueryShift));
    for (auto* candidate = zero.Def(); candidate; candidate = NextZeroInput(candidate)) {
        const auto position = analysis.InstructionPosition(candidate);
        if (position && ProveDeadUses(candidate, carry_add))
            WriteRow(*position) |= kPendingDead;
    }
    Cover(carry_test);
    Cover(carry_add);
    for (auto* candidate = zero.Def(); candidate; candidate = NextZeroInput(candidate)) {
        const auto position = analysis.InstructionPosition(candidate);
        if (!position) continue;
        const auto row = Row(*position);
        if (row != UINT32_MAX && (row >> kQueryShift) == query && (row & kPendingDead))
            Cover(candidate);
    }
    // Every selected carry has already passed the immutable PSTATE check.
    // Compare only asks whether any such test occurs after its producer.
    latest_carry_test = std::max<u32>(latest_carry_test, carry_test->Id());
    const auto first = inst->GetArg<ir::Value>(0);
    const bool left = first.Def() == value.Def() && first.Type() == value.Type();
    const auto selected = left ? first : inst->GetArg<ir::Operand>(1).GetLeft().value;
    ASSERT(selected.Def() == value.Def() && selected.Type() == value.Type());
    return IntegerFlagsAction{
            0, inst->Id(), left ? IntegerFlagsAction::CarryLeft : IntegerFlagsAction::CarryRight};
}

bool IntegerFlagsBuilder::ProveDeadUses(const ir::Inst* producer, const ir::Inst* carry_add) {
    const auto position = analysis.InstructionPosition(producer);
    ASSERT(position);
    const auto previous = Row(*position);
    if (previous != UINT32_MAX && (previous >> kQueryShift) == query)
        return ((previous >> 1) & 3) == static_cast<u32>(DeadUse::Accepted);
    // A local graph cannot prove a definition dead to another block or the
    // terminal. The visiting state also rejects cycles conservatively.
    if (analysis.DirectUseCount(producer) != producer->GetUses(false)) {
        RecordProof(*position, DeadUse::Rejected);
        return false;
    }
    RecordProof(*position, DeadUse::Visiting);
    const auto is_dead_value_op = [](ir::OpCode op) {
        switch (op) {
            case ir::OpCode::Add:
            case ir::OpCode::Sub:
            case ir::OpCode::LslImm:
            case ir::OpCode::ZeroExtend32:
            case ir::OpCode::ZeroExtend32To64:
            case ir::OpCode::BitCast:
                return true;
            default:
                return false;
        }
    };
    bool safe = true;
    for (const auto& use : analysis.Uses(producer)) {
        const auto* consumer = use.consumer;
        if (consumer == carry_add || Covered(consumer)) continue;
        if (consumer->GetOp() == ir::OpCode::SaveFlags ||
            consumer->GetOp() == ir::OpCode::BranchOnlyFlags)
            safe = Demand(producer).Null();
        else
            safe = is_dead_value_op(consumer->GetOp()) && Demand(consumer).Null() &&
                   ProveDeadUses(consumer, carry_add);
        if (!safe) break;
    }
    RecordProof(*position, safe ? DeadUse::Accepted : DeadUse::Rejected);
    return safe;
}

std::optional<CompareSelection> IntegerFlagsBuilder::MatchCompare(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::Sub || inst->GetUses() != 0) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(inst->ReturnType());
    const auto flags = Demand(inst);
    if ((width != sizeof(u8) && width != sizeof(u16)) || flags.Null() || flags.branch_only ||
        flags.clear != ir::Flags::None || False(flags.set & ir::Flags::Carry)) {
        return std::nullopt;
    }

    const auto extra_flags = flags.set & ~ir::Flags::Carry;
    if (extra_flags != ir::Flags::None) {
        if (latest_carry_test <= inst->Id()) {
            return std::nullopt;
        }
        auto pending = extra_flags;
        for (auto* scan : analysis.Between(inst->Id(), UINT32_MAX)) {
            if (pending == ir::Flags::None) break;
            if (IsFaultBoundary(scan->GetOp()) || True(ExplicitFlagRead(*scan) & pending)) {
                return std::nullopt;
            }
            const auto writer = Demand(&*scan);
            pending &= ~(writer.branch_only ? ir::Flags::All : writer.set);
            if (scan->GetOp() == ir::OpCode::ClearFlags) {
                pending &= ~scan->GetArg<ir::Flags>(0);
            } else if (scan->GetOp() == ir::OpCode::SetOverflow) {
                pending &= ~ir::Flags::Overflow;
            }
        }
        if (pending != ir::Flags::None) {
            return std::nullopt;
        }
    }

    auto left = inst->GetArg<ir::Value>(0);
    if (analysis.Contains(left.Def())) {
        const auto forwarded = std::find_if(inputs.forwarded_inputs.begin(),
                                            inputs.forwarded_inputs.end(),
                                            [&](const IntegerFlagsAction& row) {
                                                return row.IsInput() &&
                                                       row.instruction == left.Id() &&
                                                       row.argument == inst->Id();
                                            });
        if (forwarded != inputs.forwarded_inputs.end()) left = forwarded->Source(*inst);
    }
    if (!IsNarrowZeroExtended(left, width)) {
        return std::nullopt;
    }
    const auto select = [&](IntegerFlagsAction::Kind kind,
                            u32 immediate = 0,
                            const ir::Inst* load = nullptr) {
        const auto first = inst->GetArg<ir::Value>(0);
        const bool forward = first.Def() != left.Def() || first.Type() != left.Type();
        if (forward) {
            const auto source = first.Def()->GetArg<ir::Value>(0);
            ASSERT(source.Def() == left.Def() && source.Type() == left.Type());
        }
        return CompareSelection{
                {immediate, inst->Id(), kind, static_cast<u8>((width << 1) | forward)}, load};
    };
    const auto operand = inst->GetArg<ir::Operand>(1);
    if (operand.IsImm()) {
        const u64 immediate = operand.GetLeft().imm.Get();
        const u64 limit = u64{1} << (width * 8);
        if (immediate < limit && IsAddSubImmediate(immediate)) {
            return select(IntegerFlagsAction::CompareImmediate, static_cast<u32>(immediate));
        }
        return std::nullopt;
    }
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    const auto right = operand.GetLeft().value;
    if (right.Def() && right.Def()->GetOp() == ir::OpCode::LoadImm &&
        right.Def()->GetUses(false) == 1) {
        const u64 immediate = right.Def()->GetArg<ir::Imm>(0).Get();
        const u64 limit = u64{1} << (width * 8);
        if (immediate < limit && IsAddSubImmediate(immediate)) {
            return select(IntegerFlagsAction::CompareImmediate,
                          static_cast<u32>(immediate),
                          right.Def());
        }
    }
    if (!IsNarrowZeroExtended(right, width)) {
        return std::nullopt;
    }
    return select(IntegerFlagsAction::CompareValue);
}

IntegerFlagsActions IntegerFlagsBuilder::Build() {
    for (auto* inst : analysis.Instructions()) {
        if (auto action = SelectCarry(inst)) plan.actions.push_back(*action);
    }
    for (auto* inst : analysis.Instructions()) {
        if (auto selected = MatchCompare(inst)) {
            // A sole-use immediate may be defined in another Function block.
            // It belongs in final unit-ID coverage, not in local row storage.
            if (selected->immediate_load) plan.covered.push_back(selected->immediate_load->Id());
            plan.actions.push_back(selected->action);
        }
    }
    std::sort(plan.actions.begin(), plan.actions.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    std::sort(plan.covered.begin(), plan.covered.end());
    plan.covered.erase(std::unique(plan.covered.begin(), plan.covered.end()), plan.covered.end());
    return std::move(plan);
}
}  // namespace

namespace {
bool HasNarrowCandidate(const ir::Inst& inst) {
    if (inst.GetOp() != ir::OpCode::Add && inst.GetOp() != ir::OpCode::Sub) return false;
    const auto width = ir::GetValueSizeByte(inst.ReturnType());
    if (inst.GetOp() == ir::OpCode::Sub && inst.GetUses() == 0 && (width == 1 || width == 2))
        return true;
    if (inst.GetOp() != ir::OpCode::Add || width != 4) return false;
    const auto values = BinaryValues(&inst);
    if (!values) return false;
    for (const auto value : *values) {
        auto* carry = value.Def();
        if (!carry || carry->GetOp() != ir::OpCode::Add || carry->Id() + 1 != inst.Id())
            continue;
        const auto inner = BinaryValues(carry);
        if (!inner) continue;
        for (const auto input : *inner)
            if (input.Def() && input.Def()->GetOp() == ir::OpCode::TestFlags) return true;
    }
    return false;
}
}  // namespace
bool HasNarrowIntegerFlagsCandidates(const ir::Block& block) {
    for (const auto& inst : block.GetInstList())
        if (HasNarrowCandidate(inst)) return true;
    return false;
}
IntegerFlagsActions LowerNarrowIntegerFlags(compile::BlockAnalysisIndex& analysis,
                                            compile::CompileWorkspace& workspace,
                                            const IntegerFlagsInputs& inputs) {
    analysis.PrepareUses();
    return IntegerFlagsBuilder(analysis, workspace, inputs).Build();
}

namespace {
std::optional<IntegerFlagsAction> MatchNarrowFlagsInput(const ir::Inst* extract,
                                                        const ir::Inst* consumer,
                                                        compile::AllocationView allocation,
                                                        const compile::BlockAnalysisIndex& owner,
                                                        compile::PseudoFlagsView base,
                                                        const IntegerBranchAction* branch) {
    if (extract->GetOp() != ir::OpCode::BitExtract || extract->GetUses(false) != 1)
        return std::nullopt;
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if ((width != sizeof(u8) && width != sizeof(u16)) || extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8)
        return std::nullopt;
    if (!consumer ||
        (consumer->GetOp() != ir::OpCode::Add && consumer->GetOp() != ir::OpCode::Sub &&
         consumer->GetOp() != ir::OpCode::Neg) ||
        consumer->GetArg<ir::Value>(0).Def() != extract ||
        ir::GetValueSizeByte(consumer->ReturnType()) != width ||
        !True(PreparationDemand(consumer, owner, base, branch).set & ir::Flags::NZCV))
        return std::nullopt;
    const auto source = extract->GetArg<ir::Value>(0);
    // A reload lifetime ending at the extract cannot forward to its consumer.
    if (allocation.ValueType(source) == compile::AllocationView::MEM) return std::nullopt;
    // Freeze the actual operand, including the consumer's type view. A
    // selected low view keeps the original extract; otherwise read its source.
    return IntegerFlagsAction{consumer->Id(),
                              extract->Id(),
                              allocation.GPRLowView(extract->Id()) ? IntegerFlagsAction::InputLow
                                                                   : IntegerFlagsAction::Input};
}
}  // namespace
IntegerFlagsActions LowerIntegerFlags(compile::BlockAnalysisIndex& analysis,
                                      compile::CompileWorkspace& workspace,
                                      compile::AllocationView allocation,
                                      const compile::PseudoFlagsPlan& base_flags,
                                      const IntegerBranchAction* selected_branch,
                                      const BitVector& earlier_coverage,
                                      const PstateEligibility* pstate,
                                      bool canonical_carry) {
    IntegerFlagsActions prepared;
    const auto instructions = analysis.Instructions();
    bool narrow_candidates = false;
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (!narrow_candidates) narrow_candidates = HasNarrowCandidate(*instructions[i]);
        if (auto input = MatchNarrowFlagsInput(
                    instructions[i],
                    i + 1 < instructions.size() ? instructions[i + 1] : nullptr,
                    allocation,
                    analysis,
                    base_flags.View(),
                    selected_branch))
            prepared.actions.push_back(*input);
    }
    std::sort(prepared.actions.begin(), prepared.actions.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    if (!narrow_candidates) return prepared;
    IntegerFlagsInputs inputs{earlier_coverage, analysis, base_flags.View(), selected_branch};
    inputs.canonical_carry = canonical_carry;
    inputs.pstate = pstate;
    inputs.forwarded_inputs = prepared.actions;
    // Backedge/PFAF selection happens after this preparation stage. Its final
    // emitter demand must not change these qualifications or exclusion inputs.
    auto narrow = LowerNarrowIntegerFlags(analysis, workspace, inputs);
    prepared.actions.insert(prepared.actions.end(), narrow.actions.begin(), narrow.actions.end());
    prepared.covered = std::move(narrow.covered);
    std::sort(prepared.actions.begin(), prepared.actions.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    return prepared;
}
}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {
const IntegerFlagsAction* IntegerFlagsActions::Find(u32 id) const {
    const auto it =
            std::lower_bound(actions.begin(), actions.end(), id, [](const auto& row, u32 key) {
                return row.instruction < key;
            });
    return it != actions.end() && it->instruction == id ? &*it : nullptr;
}
const IntegerFlagsAction* IntegerFlagsActions::Input(u32 id) const {
    const auto* row = Find(id);
    return row && row->IsInput() ? row : nullptr;
}
const IntegerFlagsAction* IntegerFlagsActions::Carry(u32 id) const {
    const auto* row = Find(id);
    return row && row->IsCarry() ? row : nullptr;
}
const IntegerFlagsAction* IntegerFlagsActions::Compare(u32 id) const {
    const auto* row = Find(id);
    return row && row->IsCompare() ? row : nullptr;
}
bool IntegerFlagsActions::Covered(u32 id) const {
    return std::binary_search(covered.begin(), covered.end(), id);
}
}  // namespace swift::runtime::backend::arm64
