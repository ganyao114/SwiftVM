#include "translator.h"

#include "div128_narrowing_analysis.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/operand_checks.h"
#include "runtime/backend/context.h"
#include "runtime/common/div128.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

struct AuxiliaryCarryInputs {
    Register left;
    Operand right;
};

AuxiliaryCarryInputs CaptureAuxiliaryCarryInputs(
        JitContext& context, const Register& left, const Operand& right,
        const Register& result, bool needed) {
    if (!needed || (left.GetCode() != result.GetCode() &&
                    (right.IsImmediate() ||
                     right.GetRegister().GetCode() != result.GetCode()))) {
        return {left, right};
    }
    // AF needs the original operand bits even when the arithmetic overwrites
    // either input. Their XOR is sufficient and uses one temporary.
    auto captured = context.GetTmpX();
    auto& masm = context.GetMasm();
    if (right.IsImmediate()) {
        __ Mov(captured, left.X());
        if ((right.GetImmediate() >> 4) & 1) {
            __ Eor(captured, captured, 1u << 4);
        }
    } else {
        const auto shift = right.IsShiftedRegister() ? right.GetShift() : LSL;
        const auto amount = right.IsShiftedRegister() ? right.GetShiftAmount() : 0;
        __ Eor(captured, left.X(), Operand{right.GetRegister().X(), shift, amount});
    }
    return {captured, Operand{0}};
}

}  // namespace

void JitTranslator::EmitAdd(ir::Inst* inst) {
    if (auto fusion = flag_state.narrow_carry_fusions.find(inst);
        fusion != flag_state.narrow_carry_fusions.end()) {
        const auto reproved = MatchNarrowCarryFusion(inst);
        ASSERT_MSG(reproved && reproved->carry_test == fusion->second.carry_test &&
                           reproved->carry_add == fusion->second.carry_add &&
                           reproved->value.Def() == fusion->second.value.Def(),
                   "narrow carry fusion proof diverged at IR {}", inst->Id());
        ASSERT(flag_state.save_in_nzcv && flag_state.nzcv_dirty);
        const auto result = context.R(ir::Value{inst});
        const auto value = context.R(fusion->second.value, true);
        __ Adc(result.W(), value.W(), Operand{wzr});
        return;
    }
    if (auto update = pinned_gprs.pinned_load_update_instructions.find(inst);
        update != pinned_gprs.pinned_load_update_instructions.end() &&
        inst == update->second.update) {
        const auto reproved = MatchPinnedLoadUpdate(inst);
        ASSERT_MSG(reproved && *reproved == update->second,
                   "pinned load update proof diverged at IR {}", inst->Id());
        return;
    }
    auto left = inst->GetArg<ir::Value>(0);
    auto left_input = ResolveNarrowFlagsInput(left, inst);
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned = [&](ir::Value value) -> std::optional<Register> {
        return ResolvePinnedGPRUse(value, inst);
    };
    auto right_pinned = right.GetLeft().IsValue()
            ? pinned(right.GetLeft().value)
            : std::nullopt;
    const auto induction_immediate = MatchInductionImmediate(inst);
    auto right_operand = induction_immediate
            ? Operand{static_cast<s64>(*induction_immediate)}
            : (right_pinned ? Operand{*right_pinned} : EmitOperand(right));
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = FlagsResultRegister(inst, pseudo_flags);
    auto left_pinned = pinned(left_input);
    Register left_register = left_pinned ? Register{*left_pinned}
                                         : context.R(left_input, true);

    if (!pseudo_flags.Null()) {
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv && ir::GetValueSizeByte(inst->ReturnType()) <= 2) {
            // Align the architectural sign bit with W[31], perform one
            // flag-setting operation, then shift the result back down in the
            // same destination.  The shift does not alter NZCV.  Preserve an
            // input only when linear scan tied it to the destination, because
            // AF still needs the original bit 4 after the result is produced.
            Register af_left = left_register.W();
            if (context.SharesGPR(left_input, ir::Value{inst})) {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), left_register.W());
                af_left = saved.W();
            }
            if (right.GetLeft().IsValue() &&
                context.SharesGPR(right.GetLeft().value, ir::Value{inst})) {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), right_operand);
                right_operand = Operand{saved.W()};
            }
            const u32 shift = 32 - ir::GetValueSizeByte(inst->ReturnType()) * 8;
            __ Lsl(result.W(), af_left.W(), shift);
            Operand aligned_right;
            if (right_operand.IsImmediate()) {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), static_cast<u32>(right_operand.GetImmediate()));
                aligned_right = Operand{saved.W(), LSL, shift};
            } else if (!right_operand.IsShiftedRegister()) {
                aligned_right = Operand{right_operand.GetRegister().W(), LSL, shift};
            } else {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), right_operand);
                right_operand = Operand{saved.W()};
                aligned_right = Operand{saved.W(), LSL, shift};
            }
            if (!pseudo_flags.branch_only) {
                BeginFlagsTokenProducer(pseudo_flags);
            }
            __ Adds(result.W(), result.W(), aligned_right);
            if ((!pseudo_flags.branch_only && pseudo_flags.NeedsResultBits()) ||
                inst->GetUses() != 0) {
                __ Lsr(result.W(), result.W(), shift);
            }
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            if (!pseudo_flags.branch_only) {
                SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
            }
            if (!pseudo_flags.branch_only &&
                True(pseudo_flags.set & ir::Flags::Parity)) {
                SaveParity(result);
            }
            if (!pseudo_flags.branch_only &&
                True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
                SaveAuxiliaryCarry(af_left, right_operand, result);
            }
            FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
            return;
        }
        auto af_inputs = CaptureAuxiliaryCarryInputs(
                context, left_register, right_operand, result,
                !pseudo_flags.branch_only &&
                        True(pseudo_flags.set & ir::Flags::AuxiliaryCarry));
        if (needs_nzcv) {
            if (!pseudo_flags.branch_only) {
                BeginFlagsTokenProducer(pseudo_flags);
            }
            __ Adds(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            if (!pseudo_flags.branch_only) {
                SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
            }
        } else {
            // AF/PF only: use non-flag form to avoid clobbering host NZCV.
            __ Add(result, left_register, right_operand);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(af_inputs.left, af_inputs.right, result);
        }
        FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
    } else {
        __ Add(result, left_register, right_operand);
    }
}

void JitTranslator::EmitSub(ir::Inst* inst) {
    if (MatchPreIndexMemoryUpdate(inst) || MatchBiasedMemoryUpdate(inst)) {
        return;
    }
    if (auto candidate = flag_state.narrow_compares.find(inst); candidate != flag_state.narrow_compares.end()) {
        const auto reproved = MatchNarrowCompare(inst);
        ASSERT_MSG(reproved && *reproved == candidate->second,
                   "narrow compare proof diverged at IR {}", inst->Id());
        auto pseudo_flags = GetPseudoFlags(inst);
        pseudo_flags.set = ir::Flags::Carry;
        const auto result = FlagsResultRegister(inst, pseudo_flags);
        const auto pinned = ResolvePinnedGPRWUse(candidate->second.left, inst);
        const auto left = pinned ? *pinned : context.W(candidate->second.left);
        if (!pseudo_flags.branch_only) {
            BeginFlagsTokenProducer(pseudo_flags);
        }
        if (candidate->second.right) {
            __ Cmp(left, context.W(*candidate->second.right));
        } else {
            __ Cmp(left, candidate->second.immediate);
        }
        const auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
        if (!pseudo_flags.branch_only) {
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        }
        FinishFlagsTokenProducer(
                result, inst->ReturnType(), pseudo_flags, inst);
        return;
    }
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        return ResolvePinnedGPRWUse(value, inst);
    };
    if (flag_state.dead_narrow_immediate_branch &&
        flag_state.dead_narrow_immediate_branch->producer == inst) {
        const auto reproved = MatchDeadNarrowImmediateBranch(inst);
        ASSERT_MSG(reproved &&
                           reproved->producer ==
                                   flag_state.dead_narrow_immediate_branch->producer &&
                           reproved->immediate_load ==
                                   flag_state.dead_narrow_immediate_branch->immediate_load &&
                           reproved->immediate ==
                                   flag_state.dead_narrow_immediate_branch->immediate &&
                           reproved->required ==
                                   flag_state.dead_narrow_immediate_branch->required &&
                           reproved->width ==
                                   flag_state.dead_narrow_immediate_branch->width,
                   "dead narrow immediate branch proof diverged at IR {}",
                   inst->Id());
        const auto left = inst->GetArg<ir::Value>(0);
        const auto left_input = ResolveNarrowFlagsInput(left, inst);
        const auto result = context.W(ir::Value{inst});
        const auto pinned = pinned_w(left_input);
        const auto source = pinned ? *pinned : context.W(left_input);
        const bool zero_extended_load = left_input.Def() &&
                left_input.Def()->GetOp() == ir::OpCode::LoadMemory &&
                ir::GetValueSizeByte(left_input.Def()->ReturnType()) ==
                        flag_state.dead_narrow_immediate_branch->width;
        Register compare = source;
        if (!zero_extended_load) {
            if (flag_state.dead_narrow_immediate_branch->width == sizeof(u8)) {
                __ Uxtb(result, source);
            } else {
                __ Uxth(result, source);
            }
            compare = result;
        }
        __ Cmp(compare, flag_state.dead_narrow_immediate_branch->immediate);
        return;
    }
    auto left = inst->GetArg<ir::Value>(0);
    auto left_input = ResolveNarrowFlagsInput(left, inst);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_pinned = right.GetLeft().IsValue()
            ? pinned_w(right.GetLeft().value)
            : std::nullopt;
    auto right_operand = right_pinned ? Operand{*right_pinned} : EmitOperand(right);
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = FlagsResultRegister(inst, pseudo_flags);
    auto left_pinned = pinned_w(left_input);
    Register left_register = left_pinned ? Register{*left_pinned}
                                         : context.R(left_input, true);

    if (!pseudo_flags.Null()) {
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv && ir::GetValueSizeByte(inst->ReturnType()) <= 2) {
            const u32 width = ir::GetValueSizeByte(inst->ReturnType());
            auto is_zero_extended_load = [width](ir::Value value) {
                return value.Def() &&
                       value.Def()->GetOp() == ir::OpCode::LoadMemory &&
                       ir::GetValueSizeByte(value.Def()->ReturnType()) == width;
            };
            const bool branch_only_zero_compare = pseudo_flags.branch_only &&
                    pseudo_flags.set == ir::Flags::Zero &&
                    !RegionBranchPFAFActive(inst) &&
                    right.GetLeft().IsValue() && right.GetRight().Null() &&
                    IsUnmodifiedRegister(right_operand);
            if (branch_only_zero_compare) {
                const bool left_is_load = is_zero_extended_load(left_input);
                const bool right_is_load =
                        is_zero_extended_load(right.GetLeft().value);
                if (left_is_load || right_is_load) {
                    const auto right_register = right_operand.GetRegister().W();
                    const auto extend = width == sizeof(u8) ? UXTB : UXTH;
                    if (left_is_load && right_is_load) {
                        __ Cmp(left_register.W(), right_register);
                    } else if (left_is_load) {
                        __ Cmp(left_register.W(),
                               Operand{right_register, extend});
                    } else {
                        __ Cmp(right_register,
                               Operand{left_register.W(), extend});
                    }
                    FinishFlagsTokenProducer(
                            result, inst->ReturnType(), pseudo_flags, inst);
                    return;
                }
            }
            Register af_left = left_register.W();
            if (context.SharesGPR(left_input, ir::Value{inst})) {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), left_register.W());
                af_left = saved.W();
            }
            if (right.GetLeft().IsValue() &&
                context.SharesGPR(right.GetLeft().value, ir::Value{inst})) {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), right_operand);
                right_operand = Operand{saved.W()};
            }
            const u32 shift = 32 - width * 8;
            __ Lsl(result.W(), af_left.W(), shift);
            Operand aligned_right;
            if (right_operand.IsImmediate()) {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), static_cast<u32>(right_operand.GetImmediate()));
                aligned_right = Operand{saved.W(), LSL, shift};
            } else if (!right_operand.IsShiftedRegister()) {
                aligned_right = Operand{right_operand.GetRegister().W(), LSL, shift};
            } else if (right_operand.GetShift() == LSL &&
                       right_operand.GetShiftAmount() == 0) {
                aligned_right = Operand{
                        right_operand.GetRegister().W(), LSL, shift};
            } else {
                auto saved = context.GetTmpX();
                __ Mov(saved.W(), right_operand);
                right_operand = Operand{saved.W()};
                aligned_right = Operand{saved.W(), LSL, shift};
            }
            const bool region_branch_pfaf = RegionBranchPFAFActive(inst);
            if (!pseudo_flags.branch_only || region_branch_pfaf) {
                if (region_branch_pfaf) {
                    MergeNZCV();
                } else {
                    BeginFlagsTokenProducer(pseudo_flags);
                }
            }
            __ Subs(result.W(), result.W(), aligned_right);
            if ((!pseudo_flags.branch_only && pseudo_flags.NeedsResultBits()) ||
                inst->GetUses() != 0) {
                __ Lsr(result.W(), result.W(), shift);
            }
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            if (region_branch_pfaf) {
                flag_state.nzcv_requested = GuestNZCVToHost(guest_nzcv);
                flag_state.nzcv_dirty = true;
            } else if (!pseudo_flags.branch_only) {
                SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
            }
            if (!pseudo_flags.branch_only &&
                True(pseudo_flags.set & ir::Flags::Parity)) {
                SaveParity(result);
            }
            if (!pseudo_flags.branch_only &&
                True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
                SaveAuxiliaryCarry(af_left, right_operand, result);
            }
            FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
            return;
        }
        auto af_inputs = CaptureAuxiliaryCarryInputs(
                context, left_register, right_operand, result,
                !pseudo_flags.branch_only &&
                        True(pseudo_flags.set & ir::Flags::AuxiliaryCarry));
        if (needs_nzcv) {
            if (!pseudo_flags.branch_only) {
                BeginFlagsTokenProducer(pseudo_flags);
            }
            __ Subs(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            if (!pseudo_flags.branch_only) {
                SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
            }
        } else {
            __ Sub(result, left_register, right_operand);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(af_inputs.left, af_inputs.right, result);
        }
        FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
    } else {
        __ Sub(result, left_register, right_operand);
    }
}

void JitTranslator::EmitNeg(ir::Inst* inst) {
    ASSERT(context.GetFeatures().int_imm_fold);
    const auto source = inst->GetArg<ir::Value>(0);
    const auto source_input = ResolveNarrowFlagsInput(source, inst);
    auto source_reg = context.R(source_input, true);
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = FlagsResultRegister(inst, pseudo_flags);
    Register zero = result.Is64Bits() ? Register{xzr} : Register{wzr};

    Register af_source = source_reg;
    const bool save_af = !pseudo_flags.branch_only &&
                         True(pseudo_flags.set & ir::Flags::AuxiliaryCarry);
    if (save_af && context.SharesGPR(source_input, ir::Value{inst})) {
        auto saved = context.GetTmpX();
        __ Mov(saved, source_reg);
        af_source = source_reg.Is64Bits() ? saved.X() : saved.W();
    }

    const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
    if (needs_nzcv && ir::GetValueSizeByte(inst->ReturnType()) <= 2) {
        const u32 shift = 32 - ir::GetValueSizeByte(inst->ReturnType()) * 8;
        __ Lsl(result.W(), source_reg.W(), shift);
        const bool region_branch_pfaf = RegionBranchPFAFActive(inst);
        if (!pseudo_flags.branch_only || region_branch_pfaf) {
            if (region_branch_pfaf) {
                MergeNZCV();
            } else {
                BeginFlagsTokenProducer(pseudo_flags);
            }
        }
        __ Subs(result.W(), wzr, result.W());
        if ((!pseudo_flags.branch_only && pseudo_flags.NeedsResultBits()) ||
            inst->GetUses() != 0) {
            __ Lsr(result.W(), result.W(), shift);
        }
        const auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
        if (region_branch_pfaf) {
            flag_state.nzcv_requested = GuestNZCVToHost(guest_nzcv);
            flag_state.nzcv_dirty = true;
        } else if (!pseudo_flags.branch_only) {
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        }
    } else if (needs_nzcv) {
        if (!pseudo_flags.branch_only) {
            BeginFlagsTokenProducer(pseudo_flags);
        }
        __ Subs(result, zero, source_reg);
        if (!pseudo_flags.branch_only) {
            const auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        }
    } else {
        __ Neg(result, source_reg);
    }

    if (!pseudo_flags.branch_only &&
        True(pseudo_flags.set & ir::Flags::Parity)) {
        SaveParity(result);
    }
    if (save_af) {
        SaveAuxiliaryCarry(zero, Operand{af_source}, result);
    }
    FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
}

void JitTranslator::EmitAdc(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = FlagsResultRegister(inst, pseudo_flags);
    auto left_register = context.R(left, true);

    // Bring the guest carry flag into host C.
    if (!(flag_state.save_in_nzcv && flag_state.nzcv_dirty)) {
        LoadNZCVFromFlags();
    }

    if (!pseudo_flags.Null()) {
        auto af_inputs = CaptureAuxiliaryCarryInputs(
                context, left_register, right_operand, result,
                !pseudo_flags.branch_only &&
                        True(pseudo_flags.set & ir::Flags::AuxiliaryCarry));
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
            // OFF must not insert MergeNZCV: Adcs consumes live host C.
            if (FlagsRegsEnabled() && !pseudo_flags.branch_only) {
                BeginFlagsTokenProducer(pseudo_flags);
            }
            __ Adcs(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            if (!pseudo_flags.branch_only) {
                SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
            }
        } else {
            __ Adc(result, left_register, right_operand);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(af_inputs.left, af_inputs.right, result);
        }
        FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
    } else {
        __ Adc(result, left_register, right_operand);
    }
}

void JitTranslator::EmitSbb(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = FlagsResultRegister(inst, pseudo_flags);
    auto left_register = context.R(left, true);

    // The carry is stored with host (ARM) semantics, so SBC matches the guest borrow.
    if (!(flag_state.save_in_nzcv && flag_state.nzcv_dirty)) {
        LoadNZCVFromFlags();
    }

    if (!pseudo_flags.Null()) {
        auto af_inputs = CaptureAuxiliaryCarryInputs(
                context, left_register, right_operand, result,
                !pseudo_flags.branch_only &&
                        True(pseudo_flags.set & ir::Flags::AuxiliaryCarry));
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
            if (FlagsRegsEnabled() && !pseudo_flags.branch_only) {
                BeginFlagsTokenProducer(pseudo_flags);
            }
            __ Sbcs(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            if (!pseudo_flags.branch_only) {
                SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
            }
        } else {
            __ Sbc(result, left_register, right_operand);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(af_inputs.left, af_inputs.right, result);
        }
        FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
    } else {
        __ Sbc(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAnd(ir::Inst* inst) {
    if (auto masked = shift_masked_inputs.find(inst);
        masked != shift_masked_inputs.end()) {
        const auto reproved = MatchShiftMaskedInput(inst);
        ASSERT_MSG(reproved && *reproved == masked->second,
                   "shift masked input proof diverged at IR {}", inst->Id());
        __ Ubfx(context.R(ir::Value{inst}), context.R(reproved->source),
                reproved->offset, 1);
        return;
    }
    if (flag_state.dead_edge_integer_branch &&
        flag_state.dead_edge_integer_branch->producer == inst &&
        flag_state.dead_edge_integer_branch->zero_target) {
        return;
    }
    if (local_conditions.contains(inst)) {
        return;
    }
    auto left = inst->GetArg<ir::Value>(0);
    if (auto masked = narrow_masked_inputs.find(inst);
        masked != narrow_masked_inputs.end()) {
        const auto reproved = MatchNarrowMaskedInput(inst);
        ASSERT_MSG(reproved && *reproved == masked->second,
                   "narrow masked input proof diverged at IR {}", inst->Id());
        left = reproved->source;
    }
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        return ResolvePinnedGPRWUse(value, inst);
    };
    auto right_pinned = right.GetLeft().IsValue()
            ? pinned_w(right.GetLeft().value)
            : std::nullopt;
    auto right_operand = right_pinned
            ? Operand{*right_pinned}
            : (context.GetFeatures().int_imm_fold && right.IsImm() &&
                       Assembler::IsImmLogical(right.GetLeft().imm.Get(),
                                               inst->ReturnType() == ir::ValueType::U64 ||
                                                       inst->ReturnType() == ir::ValueType::S64
                                                       ? 64
                                                        : 32)
                       ? Operand{static_cast<s64>(right.GetLeft().imm.Get())}
                       : EmitOperand(right));
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = FlagsResultRegister(inst, pseudo_flags);
    auto left_pinned = pinned_w(left);
    Register left_register = left_pinned ? Register{*left_pinned} : context.R(left, true);

    if (!pseudo_flags.Null()) {
        if (!pseudo_flags.branch_only) {
            BeginFlagsTokenProducer(pseudo_flags);
        }
        // x86 logical ops: N/Z from the result, C/V cleared.
        __ Ands(result, left_register, right_operand);
        if (!pseudo_flags.branch_only) {
            if (FlagsRegsEnabled()) {
                flag_state.nzcv_requested |= GuestNZCVToHost(pseudo_flags.set & ir::Flags::NZ);
                flag_state.nzcv_dirty = true;
            } else {
                MergeLogicalFlagsNZ(pseudo_flags.set);
            }
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
    } else {
        __ And(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAddPhi(ir::Inst* inst) {}

void JitTranslator::EmitAndNot(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = FlagsResultRegister(inst, pseudo_flags);
    auto left_register = context.R(left, true);

    if (!pseudo_flags.Null()) {
        if (!pseudo_flags.branch_only) {
            BeginFlagsTokenProducer(pseudo_flags);
        }
        __ Bics(result, left_register, right_operand);
        if (!pseudo_flags.branch_only) {
            if (FlagsRegsEnabled()) {
                flag_state.nzcv_requested |= GuestNZCVToHost(pseudo_flags.set & ir::Flags::NZ);
                flag_state.nzcv_dirty = true;
            } else {
                MergeLogicalFlagsNZ(pseudo_flags.set);
            }
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        FinishFlagsTokenProducer(result, inst->ReturnType(), pseudo_flags, inst);
    } else {
        __ Bic(result, left_register, right_operand);
    }
}

void JitTranslator::EmitOr(ir::Inst* inst) {
    if (EmitFunnelShiftResult(inst)) {
        return;
    }
    if (local_conditions.contains(inst)) {
        return;
    }
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        return ResolvePinnedGPRWUse(value, inst);
    };
    auto pseudo_flags = GetPseudoFlags(inst);
    if (!pseudo_flags.Null() && inst->GetUses() == 0 && right.IsImm() &&
        right.GetLeft().imm.Get() == 0) {
        const auto pinned = pinned_w(left);
        Register value = pinned ? Register{*pinned} : context.R(left);
        if (!pseudo_flags.branch_only) {
            BeginFlagsTokenProducer(pseudo_flags);
        }
        SaveLogicalResultFlags(value, left.Type(), pseudo_flags);
        if (const auto producer = funnel_shift_parity_producers.find(inst);
            producer != funnel_shift_parity_producers.end()) {
            FinishFlagsTokenProducer(value, left.Type(), pseudo_flags, producer->second);
        }
        return;
    }
    auto right_pinned = right.GetLeft().IsValue()
            ? pinned_w(right.GetLeft().value)
            : std::nullopt;
    auto right_operand = right_pinned
            ? Operand{*right_pinned}
            : (context.GetFeatures().int_imm_fold && right.IsImm() &&
                       Assembler::IsImmLogical(
                               right.GetLeft().imm.Get(),
                               inst->ReturnType() == ir::ValueType::U64 ||
                                               inst->ReturnType() == ir::ValueType::S64
                                       ? 64
                                       : 32)
                       ? Operand{static_cast<s64>(right.GetLeft().imm.Get())}
                       : EmitOperand(right));
    auto result = context.R(ir::Value{inst});
    auto left_pinned = pinned_w(left);
    Register left_register = left_pinned ? Register{*left_pinned}
                                         : context.R(left, true);

    if (!pseudo_flags.Null() && !pseudo_flags.branch_only) {
        BeginFlagsTokenProducer(pseudo_flags);
    }
    __ Orr(result, left_register, right_operand);
    if (!pseudo_flags.Null()) {
        SaveLogicalResultFlags(result, left.Type(), pseudo_flags);
    }
}

void JitTranslator::EmitXor(ir::Inst* inst) {
    auto pseudo_flags = GetPseudoFlags(inst);
    auto result = context.R(ir::Value{inst});
    if (scalar_copy_analysis.IsSelfXor(inst)) {
        if (!pseudo_flags.Null()) {
            if (!pseudo_flags.branch_only) {
                BeginFlagsTokenProducer(pseudo_flags);
            }
            Register zero = result.Is64Bits() ? Register{xzr} : Register{wzr};
            __ Ands(result, zero, Operand{zero});
            RecordLogicalResultFlags(result, pseudo_flags);
        } else {
            __ Mov(result, 0);
        }
        return;
    }

    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        return ResolvePinnedGPRWUse(value, inst);
    };
    auto right_pinned = right.GetLeft().IsValue()
            ? pinned_w(right.GetLeft().value)
            : std::nullopt;
    auto right_operand = right_pinned
            ? Operand{*right_pinned}
            : (context.GetFeatures().int_imm_fold && right.IsImm() &&
                       Assembler::IsImmLogical(right.GetLeft().imm.Get(),
                                               inst->ReturnType() == ir::ValueType::U64 ||
                                                       inst->ReturnType() == ir::ValueType::S64
                                                       ? 64
                                                       : 32)
                       ? Operand{static_cast<s64>(right.GetLeft().imm.Get())}
                       : EmitOperand(right));
    auto left_pinned = pinned_w(left);
    Register left_register = left_pinned ? Register{*left_pinned} : context.R(left, true);

    if (!pseudo_flags.Null() && !pseudo_flags.branch_only) {
        BeginFlagsTokenProducer(pseudo_flags);
    }
    __ Eor(result, left_register, right_operand);
    if (!pseudo_flags.Null()) {
        SaveLogicalResultFlags(result, left.Type(), pseudo_flags);
    }
}

void JitTranslator::EmitNot(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.R(ir::Value{inst});
    if (inst->ArgAt(1).IsVoid()) {
        // Unary form: logical not (used for zero checks), result is 0/1.
        MergeNZCV();
        __ Cmp(context.R(value), 0);
        __ Cset(result.W(), eq);
    } else {
        auto right = inst->GetArg<ir::Operand>(1);
        __ Mvn(result, EmitOperand(right));
    }
}

void JitTranslator::EmitAsrImm(ir::Inst* inst) {
    if (fused_shift_masked_shifts.contains(inst)) {
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto asr = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Asr(result, context.R(value), asr);
}

void JitTranslator::EmitLslImm(ir::Inst* inst) {
    if (EmitFunnelShiftPart(inst)) {
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto lsl = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Lsl(result, context.R(value), lsl);
}

void JitTranslator::EmitLsrImm(ir::Inst* inst) {
    if (EmitFunnelShiftPart(inst)) {
        return;
    }
    if (fused_shift_masked_shifts.contains(inst)) {
        return;
    }
    if (auto fused = fused_narrow_extract_shifts.find(inst);
        fused != fused_narrow_extract_shifts.end()) {
        const auto candidate = narrow_extract_extensions.find(fused->second);
        const auto reproved = MatchNarrowExtractExtension(fused->second);
        ASSERT_MSG(candidate != narrow_extract_extensions.end() && reproved &&
                           *reproved == candidate->second && reproved->shift == inst,
                   "narrow extract shift proof diverged at IR {}", inst->Id());
        const u32 lsr = inst->GetArg<ir::Imm>(1).Get();
        __ Ubfx(context.W(ir::Value{inst}), context.W(reproved->source), lsr,
                reproved->width * 8 - lsr);
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto lsr = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Lsr(result, context.R(value), lsr);
}

void JitTranslator::EmitRorImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto ror = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Ror(result, context.R(value), ror);
}

void JitTranslator::EmitByteSwap(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.R(ir::Value{inst});
    const u32 width = inst->GetArg<ir::Imm>(1).Get();
    switch (width) {
        case 16:
            ASSERT_MSG(inst->ReturnType() == ir::ValueType::U16,
                       "16-bit byte swap lost its narrow-result proof");
            __ Rev16(result.W(), context.W(value));
            break;
        case 32:
            __ Rev(result.W(), context.W(value));
            break;
        case 64:
            __ Rev(result.X(), context.X(value));
            break;
        default:
            PANIC("invalid byte-swap width {}", width);
    }
}

void JitTranslator::EmitCountLeadingZeros64(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    ASSERT(value.Type() == ir::ValueType::U64);
    __ Clz(context.X(ir::Value{inst}), context.X(value));
}

void JitTranslator::EmitCountTrailingZeros64(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    ASSERT(value.Type() == ir::ValueType::U64);
    auto result = context.X(ir::Value{inst});
    __ Rbit(result, context.X(value));
    __ Clz(result, result);
}

void JitTranslator::EmitCountLeadingZeros32(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    ASSERT(value.Type() == ir::ValueType::U32);
    __ Clz(context.W(ir::Value{inst}), context.W(value));
}

void JitTranslator::EmitCountTrailingZeros32(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    ASSERT(value.Type() == ir::ValueType::U32);
    auto result = context.W(ir::Value{inst});
    __ Rbit(result, context.W(value));
    __ Clz(result, result);
}

void JitTranslator::EmitBitCast(ir::Inst* inst) {
    // Ignore
}

void JitTranslator::EmitLoadImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Imm>(0);
    if (CanUseZeroStoreRegister(ir::Value{inst})) {
        return;
    }
    const auto pinned = ResolvePinnedGPRValue(ir::Value{inst});
    auto result = pinned
            ? (ir::GetValueSizeByte(inst->ReturnType()) > sizeof(u32)
                       ? Register{*pinned}
                       : Register{pinned->W()})
            : context.R(ir::Value{inst});
    __ Mov(result, value.Get());
}



void JitTranslator::EmitDiv(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto type = left.Type();
    auto result = context.R(ir::Value{inst});
    const bool is_signed = ir::IsSignValueType(type);
    const auto size = ir::GetValueSizeByte(type);

    Register dividend = context.R(left, true);
    Register divisor = MaterializeOperand(EmitOperand(right), type);

    // NOTE: division by zero follows ARM64 host semantics (result = 0, no trap).
    // x86 guest #DE behaviour is not modelled here.
    if (size <= 2) {
        auto clean_left = context.GetTmpX();
        auto clean_right = context.GetTmpX();
        if (is_signed) {
            if (size == 1) {
                __ Sxtb(clean_left.W(), dividend.W());
                __ Sxtb(clean_right.W(), divisor.W());
            } else {
                __ Sxth(clean_left.W(), dividend.W());
                __ Sxth(clean_right.W(), divisor.W());
            }
        } else {
            if (size == 1) {
                __ Uxtb(clean_left.W(), dividend.W());
                __ Uxtb(clean_right.W(), divisor.W());
            } else {
                __ Uxth(clean_left.W(), dividend.W());
                __ Uxth(clean_right.W(), divisor.W());
            }
        }
        dividend = clean_left.W();
        divisor = clean_right.W();
    }

    if (is_signed) {
        __ Sdiv(result, dividend, divisor);
    } else {
        __ Udiv(result, dividend, divisor);
    }

    auto pseudo_flags = GetPseudoFlags(inst);
    if (!pseudo_flags.Null() && True(pseudo_flags.set & ir::Flags::CV)) {
        MergeNZCV();
        SaveCV(result, type);
    }
}

void JitTranslator::EmitSignedDiv64(ir::Inst* inst) {
    auto dividend = context.R(inst->GetArg<ir::Value>(0), true);
    auto divisor = context.R(inst->GetArg<ir::Value>(1), true);
    ASSERT(dividend.Is64Bits() && divisor.Is64Bits());
    __ Sdiv(context.X(ir::Value{inst}), dividend.X(), divisor.X());
}

void JitTranslator::EmitDiv128(ir::Inst* inst) {
    const bool sign = inst->GetArg<ir::Imm>(3).Get() != 0;
    const auto high = inst->GetArg<ir::Value>(0);
    const auto low = inst->GetArg<ir::Value>(1);
    const auto divisor = inst->GetArg<ir::Value>(2);
    if (Div128NarrowingAnalysis{cur_block}.CanLowerNatively(inst)) {
        const auto secondary_results =
                inst->GetPseudoOperations(ir::OpCode::Div128Remainder);
        ASSERT(secondary_results.size() <= 1);
        const auto low_register = context.X(low);
        const auto divisor_register = context.X(divisor);
        const auto quotient = context.X(ir::Value{inst});
        if (secondary_results.empty()) {
            if (sign) {
                __ Sdiv(quotient, low_register, divisor_register);
            } else {
                __ Udiv(quotient, low_register, divisor_register);
            }
            return;
        }

        const XRegister remainder{context.RForWrite(
                ir::Value{secondary_results.front()}).GetCode()};
        context.ReserveTmpX(remainder);
        const auto quotient_tmp = context.GetTmpX();
        if (sign) {
            __ Sdiv(quotient_tmp, low_register, divisor_register);
        } else {
            __ Udiv(quotient_tmp, low_register, divisor_register);
        }
        __ Msub(remainder, quotient_tmp, divisor_register, low_register);
        Label divisor_ready;
        __ Cbnz(divisor_register, &divisor_ready);
        __ Mov(remainder, xzr);
        __ Bind(&divisor_ready);
        __ Mov(quotient, quotient_tmp);
        return;
    }

    const auto target = sign ? &swift::runtime::DivideSigned128
                             : &swift::runtime::DivideUnsigned128;
    std::vector<ir::DataClass> args{
            high,
            low,
            divisor,
    };
    EmitPreserveAllPairCall(inst,
                            reinterpret_cast<VAddr>(target),
                            args,
                            ir::OpCode::Div128Remainder,
                            ir::HostRegisterEffect::GeneralOnly);
}

void JitTranslator::EmitDiv128Remainder(ir::Inst*) {}

void JitTranslator::EmitCpuid(ir::Inst* inst) {
    std::vector<ir::DataClass> args{
            inst->GetArg<ir::Value>(0),
            inst->GetArg<ir::Value>(1),
            inst->GetArg<ir::Imm>(2),
    };
    EmitPreserveAllPairCall(inst,
                            inst->GetArg<ir::Imm>(3).Get(),
                            args,
                            ir::OpCode::CpuidUpper,
                            ir::HostRegisterEffect::GeneralOnly);
}

void JitTranslator::EmitCpuidUpper(ir::Inst*) {}

void JitTranslator::EmitMul(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto type = left.Type();
    auto result = context.R(ir::Value{inst});
    auto pinned = left.Def() ? pinned_gprs.fused_pin_gpr_reads.find(left.Def())
                             : pinned_gprs.fused_pin_gpr_reads.end();
    Register left_register = pinned != pinned_gprs.fused_pin_gpr_reads.end()
            ? Register{WRegister(pinned->second)}
            : context.R(left, true);
    auto pseudo_flags = GetPseudoFlags(inst);
    auto right_operand = EmitOperand(right);

    // MaterializeOperand always emits a fresh Mov even when EmitOperand has
    // already produced the exact register form accepted by A64 Mul.  That
    // emitter-only temporary has one generated consumer and no IR observers,
    // so using the source register directly neither deletes an SSA definition
    // nor changes its lifetime.  Keep every shape that could carry flags,
    // require a narrow cast, come from a spill, or encode an induction/
    // composite/immediate operand on the established path.
    const auto right_part = right.GetLeft();
    const bool basic_value = right.GetRight().Null() && right_part.IsValue() &&
                             right_part.value.Defined();
    const bool unobserved_value =
            basic_value &&
            right_part.value.Def()->GetUses(false) ==
                    right_part.value.Def()->GetUses() &&
            right_part.value.Def()->GetPseudoOperations().empty();
    const bool kill_operand_copy =
            context.GetFeatures().operand_copy_kill &&
            ir::GetValueSizeByte(type) >= sizeof(u32) && pseudo_flags.Null() &&
            unobserved_value && !context.IsSpilled(right_part.value) &&
            IsUnmodifiedRegister(right_operand);
    auto multiplier = kill_operand_copy
            ? right_operand.GetRegister()
            : MaterializeOperand(right_operand, type);

    const bool is_64 = ir::GetValueSizeByte(type) == 8;
    const bool is_signed = ir::IsSignValueType(type);
    const bool want_cv = True(pseudo_flags.set & ir::Flags::CV);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
    }

    if (want_cv && !is_64) {
        // Widen the multiply so the upper half can be checked for x86 CF/OF.
        auto wide = context.GetTmpX();
        if (is_signed) {
            __ Smull(wide, left_register.W(), multiplier.W());
        } else {
            __ Umull(wide, left_register.W(), multiplier.W());
        }
        __ Mov(result, wide.W());
        if (is_signed) {
            // Overflow when the upper half is not the sign extension of the result.
            Label no_overflow;
            const auto sign_extended = context.GetSharedTmpX();
            __ Sxtw(sign_extended, wide.W());
            __ Cmp(sign_extended, wide);
            __ B(&no_overflow, eq);
            __ Orr(flags, flags, 3u << HostFlagsBit::V);
            __ Bind(&no_overflow);
        } else {
            SaveCV(wide, type);
        }
    } else {
        __ Mul(result, left_register, multiplier);
    }

    if (!pseudo_flags.Null() && True(pseudo_flags.set & ir::Flags::Parity)) {
        SaveParity(result);
    }
}

void JitTranslator::EmitMulSub(ir::Inst* inst) {
    const auto left = inst->GetArg<ir::Value>(0);
    const auto right = inst->GetArg<ir::Value>(1);
    const auto accumulator = inst->GetArg<ir::Value>(2);
    ASSERT(left.Type() == right.Type() && left.Type() == accumulator.Type());
    __ Msub(context.R(ir::Value{inst}),
            context.R(left),
            context.R(right),
            context.R(accumulator));
}

void JitTranslator::EmitCondSelect(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Cond>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    if (!(flag_state.save_in_nzcv && flag_state.nzcv_dirty)) {
        LoadNZCVFromFlags();
    }
    __ Csel(result, context.R(true_value), context.R(false_value), MapCond(cond));
}

void JitTranslator::EmitCondSet(ir::Inst* inst) {
    if (inst->GetUses() == 1) {
        for (auto& user : cur_block->GetInstList()) {
            if ((user.GetOp() == ir::OpCode::And ||
                 user.GetOp() == ir::OpCode::Or) &&
                local_conditions.contains(&user)) {
                for (auto value : user.GetValues()) {
                    if (value.Def() == inst) {
                        return;
                    }
                }
            }
        }
    }
    auto cond = inst->GetArg<ir::Cond>(0);
    // Cond is untyped, so Inst::SetArg cannot infer a return type here and a
    // front end that forgets SetType leaves it VOID.  That is silent in this
    // back end (RegAlloc keys off the opcode's meta return type) and silent
    // data loss in the interpreter, so refuse it in both.
    ASSERT(inst->ReturnType() != ir::ValueType::VOID);
    auto following = cur_block->GetInstList().iterator_to(*inst);
    if (++following == cur_block->GetInstList().end() && flag_state.save_in_nzcv &&
        flag_state.nzcv_dirty && RecordLocalCondition(inst, cond)) {
        return;
    }
    auto result = context.R(ir::Value{inst});
    if (!(flag_state.save_in_nzcv && flag_state.nzcv_dirty)) {
        if (TryEmitCondSetFromFlags(inst, cond)) {
            return;
        }
        LoadNZCVFromFlags();
    }
    __ Cset(result, MapCond(cond));
}

void JitTranslator::EmitLocalCondSet(ir::Inst* inst) {
    auto cond = DeadEdgeIntegerBranchCondition(inst).value_or(
            inst->GetArg<ir::Cond>(0));
    ASSERT(inst->ReturnType() != ir::ValueType::VOID);
    if (RecordLocalCondition(inst, cond)) {
        return;
    }
    __ Cset(context.R(ir::Value{inst}), MapCond(cond));
}

void JitTranslator::EmitLocalParitySet(ir::Inst* inst) {
    auto result = context.R(ir::Value{inst}).W();
    auto source = context.R(inst->GetArg<ir::Value>(0)).W();
    __ Mov(result, source);
    __ Eor(result, result, Operand{result, LSR, 4});
    __ Eor(result, result, Operand{result, LSR, 2});
    __ Eor(result, result, Operand{result, LSR, 1});
    const bool inverted = inst->GetArg<ir::Imm>(1).Get() != 0;
    // The folded low bit is one for odd parity. A terminal JP consumes Z=1,
    // while JNP consumes Z=0; record that condition so the terminal emits a
    // direct B.cond instead of materialising a boolean and testing it again.
    if (RecordLocalCondition(
                inst, inverted ? ir::Cond::NE : ir::Cond::EQ)) {
        __ Tst(result, 1);
        return;
    }
    __ And(result, result, 1);
    // The folded xor is 1 for odd parity. x86 PF wants even parity; the
    // immediate selects PF (0) versus !PF (1).
    const u32 invert = static_cast<u32>(inverted);
    __ Eor(result, result, 1u ^ invert);
}

void JitTranslator::EmitBranchOnlyEdges(ir::Inst* inst) {}

void JitTranslator::EmitFCmpCondSet(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Cond>(1);
    auto fcmp = inst->GetArg<ir::Value>(0);
    ASSERT(fcmp.Def() && fcmp.Def()->GetOp() == ir::OpCode::VecFCmp);
    ASSERT(inst->ReturnType() != ir::ValueType::VOID);

    if (RawFCmpCondition(fcmp.Def()) == inst) {
        if (RecordLocalCondition(inst, cond)) {
            return;
        }
        __ Cset(context.R(ir::Value{inst}), MapCond(cond));
        return;
    }

    if (IsCompactFCmp(fcmp)) {
        // AXFLAG produces inverted guest carry. An intervening InvertCarry
        // changes that representation before this consumer; follow the emitted
        // operations rather than assuming that AXFLAG's carry is still live.
        bool inverted_carry = true;
        auto& instructions = cur_block->GetInstList();
        for (auto scan = std::next(instructions.iterator_to(*fcmp.Def()));
             scan != instructions.end() && &*scan != inst; ++scan) {
            if (scan->GetOp() == ir::OpCode::InvertCarry &&
                &*scan != flag_state.raw_carry_pending) {
                inverted_carry = !inverted_carry;
            }
        }
        if (!inverted_carry && (cond == ir::Cond::GT || cond == ir::Cond::LE)) {
            // Direct CF and ZF need two predicates for JA/JBE. Keep NZCV
            // intact because later guest instructions can still observe it.
            auto result = context.R(ir::Value{inst}).W();
            __ Cset(result, cond == ir::Cond::GT ? ne : eq);
            if (cond == ir::Cond::GT) {
                __ Csel(result, result, wzr, cc);
            } else {
                __ Csinc(result, result, wzr, cc);
            }
            return;
        }
        ir::Cond mapped{};
        bool nzcv_condition = true;
        switch (cond) {
            case ir::Cond::LT:
                mapped = inverted_carry ? ir::Cond::CC : ir::Cond::CS;
                break;
            case ir::Cond::GE:
                mapped = inverted_carry ? ir::Cond::CS : ir::Cond::CC;
                break;
            case ir::Cond::GT: mapped = ir::Cond::HI; break;  // !CF && !ZF
            case ir::Cond::LE: mapped = ir::Cond::LS; break;  // CF || ZF
            case ir::Cond::VS:  // unordered = !ordered
            case ir::Cond::VC:  // ordered
                nzcv_condition = false;
                break;
            default:
                PANIC("unexpected compact FCmp condition");
        }
        if (nzcv_condition) {
            if (RecordLocalCondition(inst, mapped)) {
                return;
            }
            __ Cset(context.R(ir::Value{inst}), MapCond(mapped));
            return;
        }

        auto result = context.R(ir::Value{inst});
        Register ordered = CanUseCompactFCmpCarrier(fcmp.Def())
                ? flags.W()
                : context.R(fcmp).W();
        // The carrier can already contain committed NZCV bits. Only its low
        // bit represents ordered, including when SETcc consumes the result.
        __ And(result.W(), ordered, 1);
        if (cond == ir::Cond::VS) {
            __ Eor(result.W(), result.W(), 1);
        }
        return;
    }

    if (RecordLocalCondition(inst, cond)) {
        return;
    }
    __ Cset(context.R(ir::Value{inst}), MapCond(cond));
}

void JitTranslator::EmitZero(ir::Inst* inst) {
    auto self = ir::Value{inst};
    if (ir::IsFloatValueType(inst->ReturnType())) {
        __ Fmov(context.V(self).D(), 0.0);
    } else {
        __ Mov(context.R(self), 0);
    }
}

void JitTranslator::EmitGetResult(ir::Inst* inst) {
    auto src = inst->GetArg<ir::Value>(0);
    auto self = ir::Value{inst};
    if (!context.HasAllocation(self)) {
        return;
    }
    auto result = context.R(self);
    auto src_reg = context.R(src);
    if (result != src_reg) {
        __ Mov(result, src_reg);
    }
}

// Upper 64 bits of a 64x64 product.  Both operands must be in registers:
// SMULH/UMULH have no immediate form, so an immediate argument would have to
// be materialised anyway, and the IR therefore takes Value rather than
// Operand.  `true` on the source R() calls asks for the value in a register
// rather than a spill slot.
void JitTranslator::EmitMulHigh(ir::Inst* inst) {
    auto left = context.R(inst->GetArg<ir::Value>(0), true);
    auto right = context.R(inst->GetArg<ir::Value>(1), true);
    auto result = context.R(ir::Value{inst});
    if (inst->GetArg<ir::Imm>(2).Get() != 0) {
        __ Smulh(result.X(), left.X(), right.X());
    } else {
        __ Umulh(result.X(), left.X(), right.X());
    }
}

#undef __

}  // namespace swift::runtime::backend::arm64
