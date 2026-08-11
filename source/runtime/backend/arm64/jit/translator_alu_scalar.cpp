#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitAdd(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        if (value.Def()) {
            if (auto it = fused_pin_gpr_reads.find(value.Def());
                it != fused_pin_gpr_reads.end()) {
                return WRegister(it->second);
            }
        }
        return std::nullopt;
    };
    auto right_pinned = right.GetLeft().IsValue()
            ? pinned_w(right.GetLeft().value)
            : std::nullopt;
    const auto induction_immediate = MatchInductionImmediate(inst);
    auto right_operand = induction_immediate
            ? Operand{static_cast<s64>(*induction_immediate)}
            : (right_pinned ? Operand{*right_pinned} : EmitOperand(right));
    auto result = context.R(ir::Value{inst});
    auto left_pinned = pinned_w(left);
    Register left_register = left_pinned ? Register{*left_pinned} : context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv && ir::GetValueSizeByte(inst->ReturnType()) <= 2) {
            // Align the architectural sign bit with W[31], perform one
            // flag-setting operation, then shift the result back down in the
            // same destination.  The shift does not alter NZCV.  Preserve an
            // input only when linear scan tied it to the destination, because
            // AF still needs the original bit 4 after the result is produced.
            Register af_left = left_register.W();
            if (context.SharesGPR(left, ir::Value{inst})) {
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
                MergeNZCV();
            }
            __ Adds(result.W(), result.W(), aligned_right);
            __ Lsr(result.W(), result.W(), shift);
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
            return;
        }
        if (needs_nzcv) {
            if (!pseudo_flags.branch_only) {
                MergeNZCV();
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
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Add(result, left_register, right_operand);
    }
}

void JitTranslator::EmitSub(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        if (value.Def()) {
            if (auto it = fused_pin_gpr_reads.find(value.Def());
                it != fused_pin_gpr_reads.end()) {
                return WRegister(it->second);
            }
        }
        return std::nullopt;
    };
    auto right_pinned = right.GetLeft().IsValue()
            ? pinned_w(right.GetLeft().value)
            : std::nullopt;
    auto right_operand = right_pinned ? Operand{*right_pinned} : EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_pinned = pinned_w(left);
    Register left_register = left_pinned ? Register{*left_pinned} : context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv && ir::GetValueSizeByte(inst->ReturnType()) <= 2) {
            Register af_left = left_register.W();
            if (context.SharesGPR(left, ir::Value{inst})) {
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
            const bool region_branch_pfaf = RegionBranchPFAFActive(inst);
            if (!pseudo_flags.branch_only || region_branch_pfaf) {
                MergeNZCV();
            }
            __ Subs(result.W(), result.W(), aligned_right);
            __ Lsr(result.W(), result.W(), shift);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            if (region_branch_pfaf) {
                nzcv_requested = GuestNZCVToHost(guest_nzcv);
                nzcv_dirty = true;
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
            return;
        }
        if (needs_nzcv) {
            if (!pseudo_flags.branch_only) {
                MergeNZCV();
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
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Sub(result, left_register, right_operand);
    }
}

void JitTranslator::EmitNeg(ir::Inst* inst) {
    ASSERT(context.GetFeatures().int_imm_fold);
    const auto source = inst->GetArg<ir::Value>(0);
    auto source_reg = context.R(source, true);
    auto result = context.R(ir::Value{inst});
    Register zero = result.Is64Bits() ? Register{xzr} : Register{wzr};
    auto pseudo_flags = GetPseudoFlags(inst);

    Register af_source = source_reg;
    const bool save_af = !pseudo_flags.branch_only &&
                         True(pseudo_flags.set & ir::Flags::AuxiliaryCarry);
    if (save_af && context.SharesGPR(source, ir::Value{inst})) {
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
            MergeNZCV();
        }
        __ Subs(result.W(), wzr, result.W());
        __ Lsr(result.W(), result.W(), shift);
        const auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
        if (region_branch_pfaf) {
            nzcv_requested = GuestNZCVToHost(guest_nzcv);
            nzcv_dirty = true;
        } else if (!pseudo_flags.branch_only) {
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        }
    } else if (needs_nzcv) {
        if (!pseudo_flags.branch_only) {
            MergeNZCV();
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
}

void JitTranslator::EmitAdc(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    // Bring the guest carry flag into host C.
    if (!(save_in_nzcv && nzcv_dirty)) {
        LoadNZCVFromFlags();
    }

    if (!pseudo_flags.Null()) {
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
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
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Adc(result, left_register, right_operand);
    }
}

void JitTranslator::EmitSbb(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    // The carry is stored with host (ARM) semantics, so SBC matches the guest borrow.
    if (!(save_in_nzcv && nzcv_dirty)) {
        LoadNZCVFromFlags();
    }

    if (!pseudo_flags.Null()) {
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
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
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Sbc(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAnd(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        if (value.Def()) {
            if (auto it = fused_pin_gpr_reads.find(value.Def());
                it != fused_pin_gpr_reads.end()) {
                return WRegister(it->second);
            }
        }
        return std::nullopt;
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
    auto result = context.R(ir::Value{inst});
    auto left_pinned = pinned_w(left);
    Register left_register = left_pinned ? Register{*left_pinned} : context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        if (!pseudo_flags.branch_only) {
            MergeNZCV();
        }
        // x86 logical ops: N/Z from the result, C/V cleared.
        __ Ands(result, left_register, right_operand);
        if (!pseudo_flags.branch_only) {
            MergeLogicalFlagsNZ(pseudo_flags.set);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
    } else {
        __ And(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAddPhi(ir::Inst* inst) {}

void JitTranslator::EmitAndNot(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        if (!pseudo_flags.branch_only) {
            MergeNZCV();
        }
        __ Bics(result, left_register, right_operand);
        if (!pseudo_flags.branch_only) {
            MergeLogicalFlagsNZ(pseudo_flags.set);
        }
        if (!pseudo_flags.branch_only &&
            True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
    } else {
        __ Bic(result, left_register, right_operand);
    }
}

void JitTranslator::EmitOr(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = context.GetFeatures().int_imm_fold && right.IsImm() &&
                                 Assembler::IsImmLogical(
                                         right.GetLeft().imm.Get(),
                                         inst->ReturnType() == ir::ValueType::U64 ||
                                                         inst->ReturnType() == ir::ValueType::S64
                                                 ? 64
                                                 : 32)
            ? Operand{static_cast<s64>(right.GetLeft().imm.Get())}
            : EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null() && !pseudo_flags.branch_only) {
        MergeNZCV();
    }
    __ Orr(result, left_register, right_operand);
    if (!pseudo_flags.Null()) {
        SaveLogicalResultFlags(result, left.Type(), pseudo_flags);
    }
}

void JitTranslator::EmitXor(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto pinned_w = [&](ir::Value value) -> std::optional<WRegister> {
        if (value.Def()) {
            if (auto it = fused_pin_gpr_reads.find(value.Def());
                it != fused_pin_gpr_reads.end()) {
                return WRegister(it->second);
            }
        }
        return std::nullopt;
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
    auto result = context.R(ir::Value{inst});
    auto left_pinned = pinned_w(left);
    Register left_register = left_pinned ? Register{*left_pinned} : context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null() && !pseudo_flags.branch_only) {
        MergeNZCV();
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
    auto value = inst->GetArg<ir::Value>(0);
    auto asr = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Asr(result, context.R(value), asr);
}

void JitTranslator::EmitLslImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto lsl = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Lsl(result, context.R(value), lsl);
}

void JitTranslator::EmitLsrImm(ir::Inst* inst) {
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

void JitTranslator::EmitBitCast(ir::Inst* inst) {
    // Ignore
}

void JitTranslator::EmitLoadImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Imm>(0);
    if (context.GetFeatures().zero_store_zr && value.Get() == 0 &&
        inst->ReturnType() != ir::ValueType::VOID &&
        !ir::IsFloatValueType(inst->ReturnType()) &&
        ir::GetValueSizeByte(inst->ReturnType()) <= sizeof(u64) &&
        inst->GetUses(false) == 1 && !context.IsSpilled(ir::Value{inst})) {
        bool sole_use_is_plain_store = false;
        for (auto& use : cur_block->GetInstList()) {
            if (use.GetOp() == ir::OpCode::StoreUniform &&
                use.GetArg<ir::Value>(1).Def() == inst) {
                sole_use_is_plain_store = true;
                break;
            }
            if (use.GetOp() == ir::OpCode::StoreMemory &&
                use.GetArg<ir::Value>(1).Def() == inst) {
                sole_use_is_plain_store = true;
                break;
            }
        }
        if (sole_use_is_plain_store) {
            return;
        }
    }
    auto result = context.R(ir::Value{inst});
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

void JitTranslator::EmitMul(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto type = left.Type();
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);
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
    const bool plain_value = right.GetRight().Null() && right_part.IsValue() &&
                             right_part.value.Defined();
    const bool unobserved_value =
            plain_value &&
            right_part.value.Def()->GetUses(false) ==
                    right_part.value.Def()->GetUses() &&
            right_part.value.Def()->GetPseudoOperations().empty();
    const bool kill_operand_copy =
            context.GetFeatures().operand_copy_kill &&
            ir::GetValueSizeByte(type) >= sizeof(u32) && pseudo_flags.Null() &&
            unobserved_value && !context.IsSpilled(right_part.value) &&
            right_operand.IsPlainRegister();
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

void JitTranslator::EmitSelect(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    if (auto local = LocalConditionFor(cond)) {
        __ Csel(result, context.R(true_value), context.R(false_value), *local);
        return;
    }
    MergeNZCV();
    __ Cmp(context.W(cond), 0);
    __ Csel(result, context.R(true_value), context.R(false_value), ne);
}

void JitTranslator::EmitCondSelect(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Cond>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    if (!(save_in_nzcv && nzcv_dirty)) {
        LoadNZCVFromFlags();
    }
    __ Csel(result, context.R(true_value), context.R(false_value), MapCond(cond));
}

// Condition -> 0/1 in one instruction.  The NZCV handling is deliberately
// byte-for-byte the same as EmitCondSelect above: CSET and CSEL read the same
// host flags, so if the two diverged, replacing a CondSelect with a CondSet in
// the front end would silently change which flag state the condition sees.
// Kept in step with Interpreter::RunCondSet.
void JitTranslator::EmitCondSet(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Cond>(0);
    // Cond is untyped, so Inst::SetArg cannot infer a return type here and a
    // front end that forgets SetType leaves it VOID.  That is silent in this
    // back end (RegAlloc keys off the opcode's meta return type) and silent
    // data loss in the interpreter, so refuse it in both.
    ASSERT(inst->ReturnType() != ir::ValueType::VOID);
    auto result = context.R(ir::Value{inst});
    if (!(save_in_nzcv && nzcv_dirty)) {
        LoadNZCVFromFlags();
    }
    __ Cset(result, MapCond(cond));
}

void JitTranslator::EmitLocalCondSet(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Cond>(0);
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

    if (IsCompactFCmp(fcmp)) {
        // PublishFCmpFlags has already applied AXFLAG.  Re-express W38's raw
        // FCMP conditions over that x86-shaped NZCV so terminal B.cond/CSEL
        // fusion remains available.
        ir::Cond mapped{};
        bool nzcv_condition = true;
        switch (cond) {
            case ir::Cond::LT: mapped = ir::Cond::CC; break;  // x86 CF
            case ir::Cond::GE: mapped = ir::Cond::CS; break;  // !CF
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
        auto ordered = context.R(fcmp);
        if (cond == ir::Cond::VS) {
            __ Eor(result.W(), ordered.W(), 1);
        } else if (result.GetCode() != ordered.GetCode()) {
            __ Mov(result.W(), ordered.W());
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
