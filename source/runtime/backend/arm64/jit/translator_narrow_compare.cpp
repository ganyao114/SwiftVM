#include "translator.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {

namespace {

ir::Flags ConditionFlags(ir::Cond cond) {
    switch (cond) {
        case ir::Cond::EQ:
        case ir::Cond::NE: return ir::Flags::Zero;
        case ir::Cond::CS:
        case ir::Cond::CC: return ir::Flags::Carry;
        case ir::Cond::MI:
        case ir::Cond::PL: return ir::Flags::Negate;
        case ir::Cond::VS:
        case ir::Cond::VC: return ir::Flags::Overflow;
        case ir::Cond::HI:
        case ir::Cond::LS: return ir::Flags::Carry | ir::Flags::Zero;
        case ir::Cond::GE:
        case ir::Cond::LT: return ir::Flags::Negate | ir::Flags::Overflow;
        case ir::Cond::GT:
        case ir::Cond::LE:
            return ir::Flags::Negate | ir::Flags::Overflow | ir::Flags::Zero;
        case ir::Cond::AL:
        case ir::Cond::NV: return ir::Flags::None;
    }
    return ir::Flags::All;
}

ir::Flags ExplicitFlagRead(const ir::Inst& inst) {
    switch (inst.GetOp()) {
        case ir::OpCode::TestFlags:
        case ir::OpCode::TestNotFlags:
            return inst.GetArg<ir::Flags>(0);
        case ir::OpCode::Adc:
        case ir::OpCode::Sbb:
        case ir::OpCode::InvertCarry:
            return ir::Flags::Carry;
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
            return ir::Flags::All;
        case ir::OpCode::GetFlags:
            return inst.GetArg<ir::Flags>(1);
        case ir::OpCode::BranchOnlyFlags:
            return inst.GetArg<ir::Flags>(1);
        case ir::OpCode::CondSelect:
        case ir::OpCode::CondSet:
        case ir::OpCode::LocalCondSet:
            return ConditionFlags(inst.GetArg<ir::Cond>(0));
        default:
            return ir::Flags::None;
    }
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

}  // namespace

bool JitTranslator::IsNarrowZeroExtended(ir::Value value, u32 width) const {
    if (!value.Def() || (width != sizeof(u8) && width != sizeof(u16))) {
        return false;
    }
    auto* inst = value.Def();
    if (inst->GetOp() == ir::OpCode::LoadMemory ||
        inst->GetOp() == ir::OpCode::LoadUniform ||
        inst->GetOp() == ir::OpCode::GetHostGPR) {
        return ir::GetValueSizeByte(inst->ReturnType()) == width;
    }
    if (inst->GetOp() == ir::OpCode::ZeroExtend32 ||
        inst->GetOp() == ir::OpCode::ZeroExtend32To64 ||
        inst->GetOp() == ir::OpCode::BitCast) {
        return IsNarrowZeroExtended(inst->GetArg<ir::Value>(0), width);
    }
    return inst->GetOp() == ir::OpCode::BitExtract &&
           ir::GetValueSizeByte(inst->ReturnType()) == width &&
           inst->GetArg<ir::Imm>(1).Get() == 0 &&
           inst->GetArg<ir::Imm>(2).Get() == width * 8;
}

std::optional<JitTranslator::NarrowComparison>
JitTranslator::MatchNarrowCompare(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::Sub ||
        inst->GetUses() != 0 || RegionBranchPFAFActive(inst)) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(inst->ReturnType());
    const auto flags = GetPseudoFlags(inst);
    if ((width != sizeof(u8) && width != sizeof(u16)) || flags.Null() ||
        flags.branch_only || flags.clear != ir::Flags::None ||
        False(flags.set & ir::Flags::Carry)) {
        return std::nullopt;
    }

    const auto extra_flags = flags.set & ~ir::Flags::Carry;
    if (extra_flags != ir::Flags::None) {
        const bool feeds_fused_carry = std::ranges::any_of(
                flag_state.narrow_carry_fusions, [&](const auto& fusion) {
                    return fusion.second.carry_test->Id() > inst->Id() &&
                           CarryCanStayInPstate(fusion.second.carry_test);
                });
        if (!feeds_fused_carry) {
            return std::nullopt;
        }
        auto pending = extra_flags;
        auto scan = std::next(cur_block->GetInstList().iterator_to(*inst));
        for (; scan != cur_block->GetInstList().end() && pending != ir::Flags::None;
             ++scan) {
            if (IsFaultBoundary(scan->GetOp()) ||
                True(ExplicitFlagRead(*scan) & pending)) {
                return std::nullopt;
            }
            const auto writer = GetPseudoFlags(&*scan);
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

    const auto left = ResolveNarrowFlagsInput(
            inst->GetArg<ir::Value>(0), inst);
    if (!IsNarrowZeroExtended(left, width)) {
        return std::nullopt;
    }
    const auto operand = inst->GetArg<ir::Operand>(1);
    if (operand.IsImm()) {
        const u64 immediate = operand.GetLeft().imm.Get();
        const u64 limit = u64{1} << (width * 8);
        if (immediate < limit && masm.IsImmAddSub(immediate)) {
            return NarrowComparison{
                    .left = left,
                    .immediate = static_cast<u32>(immediate),
                    .width = static_cast<u8>(width),
            };
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
        if (immediate < limit && masm.IsImmAddSub(immediate)) {
            return NarrowComparison{
                    .left = left,
                    .immediate_load = right.Def(),
                    .immediate = static_cast<u32>(immediate),
                    .width = static_cast<u8>(width),
            };
        }
    }
    if (!IsNarrowZeroExtended(right, width)) {
        return std::nullopt;
    }
    return NarrowComparison{
            .left = left,
            .right = right,
            .width = static_cast<u8>(width),
    };
}

void JitTranslator::PrepareNarrowCompares(ir::Block* block) {
    flag_state.narrow_compares.clear();
    for (auto& inst : block->GetInstList()) {
        auto recipe = MatchNarrowCompare(&inst);
        if (!recipe) {
            continue;
        }
        if (recipe->immediate_load) {
            disable_instructions.set(recipe->immediate_load->Id());
        }
        flag_state.narrow_compares.emplace(&inst, *recipe);
    }
}

}  // namespace swift::runtime::backend::arm64
