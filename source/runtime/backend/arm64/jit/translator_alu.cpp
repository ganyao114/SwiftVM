#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

bool GcmPclMul2Enabled(const FeatureSet& features) {
    // This is a targeted fast path for the high-high PCLMUL selector used by
    // OpenSSL's Karatsuba GHASH fold.  Keep an exact process-level fallback
    // while defaulting to the architecturally equivalent PMULL2 instruction.
    return features.x86_gcm_pclmul2;
}

ir::Value ResolveWidthChainBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsKnownWidthChainWWrite(ir::Value value, const JitContext& context) {
    value = ResolveWidthChainBitCast(value);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return (context.GetFeatures().ra_width_chain ||
                context.GetFeatures().ra_width_chain_long) &&
               context.IsWidthChainCoalesced(value.Id()) &&
               context.WidthChainAnchor(value.Id()) == value.Id() &&
               ir::GetValueSizeByte(value.Type()) == sizeof(u32);
    }
    if (def->GetOp() == ir::OpCode::BitExtract &&
        ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
        def->GetArg<ir::Imm>(1).Get() == 0 &&
        def->GetArg<ir::Imm>(2).Get() == 32) {
        return IsKnownWidthChainWWrite(def->GetArg<ir::Value>(0), context);
    }
    if (def->GetOp() == ir::OpCode::ZeroExtend32To64 &&
        ir::GetValueSizeByte(def->GetArg<ir::Value>(0).Type()) == sizeof(u32)) {
        return IsKnownWidthChainWWrite(def->GetArg<ir::Value>(0), context);
    }
    return ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32);
}

bool TerminalUsesWidthChainValue(const ir::Terminal& terminal_value,
                                 ir::Inst* definition) {
    return VisitVariant<bool>(terminal_value, [&](const auto& edge) {
        using T = std::decay_t<decltype(edge)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            return ResolveWidthChainBitCast(edge.cond).Def() == definition ||
                   TerminalUsesWidthChainValue(edge.then_, definition) ||
                   TerminalUsesWidthChainValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            if (ResolveWidthChainBitCast(edge.value).Def() == definition) {
                return true;
            }
            return std::any_of(edge.cases.begin(), edge.cases.end(),
                               [&](const auto& item) {
                                   return TerminalUsesWidthChainValue(item.then, definition);
                               });
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            return TerminalUsesWidthChainValue(edge.then_, definition) ||
                   TerminalUsesWidthChainValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return TerminalUsesWidthChainValue(edge.else_, definition);
        }
        return false;
    });
}

}  // namespace

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
    auto right_operand = right_pinned ? Operand{*right_pinned} : EmitOperand(right);
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
    auto right_operand = EmitOperand(right);
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
    auto right_operand = right_pinned ? Operand{*right_pinned} : EmitOperand(right);
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

void JitTranslator::EmitVec4Or(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Orr(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitBitCast(ir::Inst* inst) {
    // Ignore
}

void JitTranslator::EmitLoadImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Imm>(0);
    auto result = context.R(ir::Value{inst});
    __ Mov(result, value.Get());
}

void JitTranslator::EmitVec4Add(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Add(result.V4S(), left.V4S(), right.V4S());
}

void JitTranslator::EmitVec4And(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ And(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVec4Mul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Mul(result.V4S(), left.V4S(), right.V4S());
}

void JitTranslator::EmitVec4Sub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Sub(result.V4S(), left.V4S(), right.V4S());
}

void JitTranslator::EmitVecXor(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Eor(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecAnd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ And(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecOr(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Orr(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecAndNot(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Bic(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecAdd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Add(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Add(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Add(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Add(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecSub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Sub(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Sub(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Sub(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Sub(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecCmpEq(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Cmeq(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Cmeq(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Cmeq(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Cmeq(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecCmpGt(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Cmgt(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Cmgt(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Cmgt(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Cmgt(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecAvg(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Urhadd(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Urhadd(result.V8H(), left.V8H(), right.V8H());
            break;
        default:
            PANIC("invalid vector average lane width");
    }
}

void JitTranslator::EmitVecMin(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (is_signed)
                __ Smin(result.V16B(), left.V16B(), right.V16B());
            else
                __ Umin(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (is_signed)
                __ Smin(result.V8H(), left.V8H(), right.V8H());
            else
                __ Umin(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (is_signed)
                __ Smin(result.V4S(), left.V4S(), right.V4S());
            else
                __ Umin(result.V4S(), left.V4S(), right.V4S());
            break;
        default:
            PANIC("invalid vector min lane width");
    }
}

void JitTranslator::EmitVecMax(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (is_signed)
                __ Smax(result.V16B(), left.V16B(), right.V16B());
            else
                __ Umax(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (is_signed)
                __ Smax(result.V8H(), left.V8H(), right.V8H());
            else
                __ Umax(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (is_signed)
                __ Smax(result.V4S(), left.V4S(), right.V4S());
            else
                __ Umax(result.V4S(), left.V4S(), right.V4S());
            break;
        default:
            PANIC("invalid vector max lane width");
    }
}

void JitTranslator::EmitVecMul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Mul(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Mul(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Mul(result.V4S(), left.V4S(), right.V4S());
            break;
        default:
            PANIC("invalid vector multiply lane width");
    }
}

void JitTranslator::EmitVecMulHigh16(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto low = context.GetTmpV();
    auto high = context.GetTmpV();
    const bool signed_lanes = inst->GetArg<ir::Imm>(2).Get() != 0;
    if (signed_lanes) {
        __ Smull(low.V4S(), left.V4H(), right.V4H());
        __ Smull2(high.V4S(), left.V8H(), right.V8H());
    } else {
        __ Umull(low.V4S(), left.V4H(), right.V4H());
        __ Umull2(high.V4S(), left.V8H(), right.V8H());
    }
    // Signedness only changes how the 16-bit operands are widened.  Once the
    // complete 32-bit products exist, both PMULHW and PMULHUW select bits 31:16.
    __ Shrn(result.V4H(), low.V4S(), 16);
    __ Shrn2(result.V8H(), high.V4S(), 16);
}

// Widening multiply of the even lanes: destination lane i is the full
// 2*src_bits product of source lane 2i of each operand.
//
// AArch64's UMULL/SMULL widen the CONTIGUOUS low half of the lanes (UMULL2 the
// high half), which is a different lane selection from this opcode's, so the
// even lanes are compacted first.  UZP1 Vt.4S, Vn.4S, Vn.4S yields
// {n0, n2, n0, n2}; UMULL then reads its low two lanes, which are exactly the
// even lanes wanted.  UMULL2 is deliberately NOT used -- it would pair lanes
// 2 and 3, i.e. one even and one odd lane, and silently produce the wrong
// second result lane.
//
// Three host instructions and two vector temporaries.  `result` is only
// written by the last one, so it may share a physical register with either
// source without the compaction clobbering an operand that is still needed.
void JitTranslator::EmitVecMulWiden(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto even_left = context.GetTmpV();
    auto even_right = context.GetTmpV();
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Uzp1(even_left.V16B(), left.V16B(), left.V16B());
            __ Uzp1(even_right.V16B(), right.V16B(), right.V16B());
            if (is_signed) {
                __ Smull(result.V8H(), even_left.V8B(), even_right.V8B());
            } else {
                __ Umull(result.V8H(), even_left.V8B(), even_right.V8B());
            }
            break;
        case 16:
            __ Uzp1(even_left.V8H(), left.V8H(), left.V8H());
            __ Uzp1(even_right.V8H(), right.V8H(), right.V8H());
            if (is_signed) {
                __ Smull(result.V4S(), even_left.V4H(), even_right.V4H());
            } else {
                __ Umull(result.V4S(), even_left.V4H(), even_right.V4H());
            }
            break;
        case 32:
            __ Uzp1(even_left.V4S(), left.V4S(), left.V4S());
            __ Uzp1(even_right.V4S(), right.V4S(), right.V4S());
            if (is_signed) {
                __ Smull(result.V2D(), even_left.V2S(), even_right.V2S());
            } else {
                __ Umull(result.V2D(), even_left.V2S(), even_right.V2S());
            }
            break;
        default:
            PANIC("invalid widening vector multiply source lane width");
    }
}

void JitTranslator::EmitVecAbsDiffSum8(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto low = context.GetTmpV();
    auto high = context.GetTmpV();
    __ Uabdl(low.V8H(), left.V8B(), right.V8B());
    __ Uabdl2(high.V8H(), left.V16B(), right.V16B());
    __ Addv(low.H(), low.V8H());
    __ Addv(high.H(), high.V8H());
    __ Eor(result.V16B(), result.V16B(), result.V16B());
    __ Ins(result.V8H(), 0, low.V8H(), 0);
    __ Ins(result.V8H(), 4, high.V8H(), 0);
}

void JitTranslator::EmitVecMadd16(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto low = context.GetTmpV();
    auto high = context.GetTmpV();
    __ Smull(low.V4S(), left.V4H(), right.V4H());
    __ Smull2(high.V4S(), left.V8H(), right.V8H());
    __ Addp(result.V4S(), low.V4S(), high.V4S());
}

namespace {

VRegister VecLaneFormat(VRegister reg, u32 lane_bits) {
    switch (lane_bits) {
        case 8:
            return reg.V16B();
        case 16:
            return reg.V8H();
        case 32:
            return reg.V4S();
        case 64:
            return reg.V2D();
        default:
            PANIC("invalid vector shift lane width");
    }
    return reg;
}

}  // namespace

void JitTranslator::EmitVecSatAdd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool signed_lanes = inst->GetArg<ir::Imm>(3).Get() != 0;
    if (signed_lanes)
        __ Sqadd(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
    else
        __ Uqadd(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
}

void JitTranslator::EmitVecSatSub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool signed_lanes = inst->GetArg<ir::Imm>(3).Get() != 0;
    if (signed_lanes)
        __ Sqsub(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
    else
        __ Uqsub(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
}

void JitTranslator::EmitVecPack(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 source_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool unsigned_destination = inst->GetArg<ir::Imm>(3).Get() != 0;
    if (source_bits == 16) {
        if (unsigned_destination) {
            __ Sqxtun(result.V8B(), left.V8H());
            __ Sqxtun2(result.V16B(), right.V8H());
        } else {
            __ Sqxtn(result.V8B(), left.V8H());
            __ Sqxtn2(result.V16B(), right.V8H());
        }
    } else if (unsigned_destination) {
        // PACKUSDW: signed 32-bit sources saturated into the UNSIGNED 16-bit
        // range, which is exactly SQXTUN. This branch had been missing, so a
        // 32-bit unsigned pack silently took the signed path below and turned
        // 0x8000..0xFFFF into 0x7FFF — and diverged from RunVecPack, which
        // honours the flag at every width. No decoder emits it yet (legacy
        // packusdw is unimplemented), so this was latent rather than live.
        __ Sqxtun(result.V4H(), left.V4S());
        __ Sqxtun2(result.V8H(), right.V4S());
    } else {
        __ Sqxtn(result.V4H(), left.V4S());
        __ Sqxtn2(result.V8H(), right.V4S());
    }
}

void JitTranslator::EmitVecShiftLeft(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto count = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto clamped = context.GetTmpX();
    auto shifts = context.GetTmpV();
    __ Mov(clamped, lane_bits);
    __ Cmp(count, clamped);
    __ Csel(clamped, count, clamped, ls);
    auto shift_format = VecLaneFormat(shifts, lane_bits);
    __ Dup(shift_format, lane_bits == 64 ? Register(clamped) : Register(clamped.W()));
    __ Ushl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), shift_format);
}

void JitTranslator::EmitVecShiftRight(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto count = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto clamped = context.GetTmpX();
    auto shifts = context.GetTmpV();
    __ Mov(clamped, lane_bits);
    __ Cmp(count, clamped);
    __ Csel(clamped, count, clamped, ls);
    __ Neg(clamped, clamped);
    auto shift_format = VecLaneFormat(shifts, lane_bits);
    __ Dup(shift_format, lane_bits == 64 ? Register(clamped) : Register(clamped.W()));
    __ Ushl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), shift_format);
}

void JitTranslator::EmitVecShiftRightArithmetic(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto count = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto clamped = context.GetTmpX();
    auto shifts = context.GetTmpV();
    __ Mov(clamped, lane_bits - 1);
    __ Cmp(count, clamped);
    __ Csel(clamped, count, clamped, ls);
    __ Neg(clamped, clamped);
    auto shift_format = VecLaneFormat(shifts, lane_bits);
    __ Dup(shift_format, lane_bits == 64 ? Register(clamped) : Register(clamped.W()));
    __ Sshl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), shift_format);
}

void JitTranslator::EmitVecShiftLeftImm(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    const u32 count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (count == 0) {
        if (result.GetCode() != value.GetCode()) {
            __ Orr(result.V16B(), value.V16B(), value.V16B());
        }
    } else if (count >= lane_bits) {
        __ Eor(result.V16B(), result.V16B(), result.V16B());
    } else {
        __ Shl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), count);
    }
}

void JitTranslator::EmitVecShiftRightImm(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    const u32 count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (count == 0) {
        if (result.GetCode() != value.GetCode()) {
            __ Orr(result.V16B(), value.V16B(), value.V16B());
        }
    } else if (count >= lane_bits) {
        __ Eor(result.V16B(), result.V16B(), result.V16B());
    } else {
        __ Ushr(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), count);
    }
}

void JitTranslator::EmitVecShiftRightArithmeticImm(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    const u32 raw_count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 count = std::min(raw_count, lane_bits - 1);
    if (count == 0) {
        if (result.GetCode() != value.GetCode()) {
            __ Orr(result.V16B(), value.V16B(), value.V16B());
        }
    } else {
        __ Sshr(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), count);
    }
}

void JitTranslator::EmitVecByteShift(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto zero = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 count = inst->GetArg<ir::Imm>(2).Get();
    const bool left = inst->GetArg<ir::Imm>(3).Get() != 0;
    ASSERT(count > 0 && count < 16);
    if (left) {
        __ Ext(result.V16B(), zero.V16B(), value.V16B(), 16 - count);
    } else {
        __ Ext(result.V16B(), value.V16B(), zero.V16B(), count);
    }
}

void JitTranslator::EmitVecShuffle32(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    auto tmp = context.GetTmpX();
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 output_lane = byte / 4;
        const u32 selected_lane = (control >> (output_lane * 2)) & 3;
        const u8 index = selected_lane * 4 + (byte & 3);
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

bool JitTranslator::ReproveShufpsImmTie(ir::Inst* inst) const {
    if (!sse_shufps_imm || !inst ||
        inst->GetOp() != ir::OpCode::VecShuffle32TwoSrc) {
        return false;
    }
    const u32 control = inst->GetArg<ir::Imm>(2).Get() & 0xffu;
    auto resolve = [](ir::Value value) {
        while (value.Defined() && value.Def()->IsBitCastOperation()) {
            value = value.Def()->GetArg<ir::Value>(0);
        }
        return value;
    };
    auto left = resolve(inst->GetArg<ir::Value>(0));
    const bool alias_shape = inst->GetArg<ir::Value>(0).Id() ==
                             inst->GetArg<ir::Value>(1).Id();
    if (control != 0xe4 &&
        !((control == 0xe0 || control == 0xe5) && alias_shape)) {
        return false;
    }
    if (!left.Defined() || !left.Def() || context.IsHostReadCoalesced(left.Id()) ||
        !context.SharesFPR(left, ir::Value{inst})) {
        return false;
    }
    u32 last_use = left.Def()->Id();
    for (auto& scan : cur_block->GetInstList()) {
        for (auto use : scan.GetValues()) {
            if (resolve(use).Def() == left.Def()) {
                last_use = std::max<u32>(last_use, scan.Id());
            }
        }
    }
    return last_use == inst->Id();
}

void JitTranslator::EmitVecShuffle32TwoSrc(ir::Inst* inst) {
    const auto left_value = inst->GetArg<ir::Value>(0);
    const auto right_value = inst->GetArg<ir::Value>(1);
    auto left = context.V(left_value);
    auto right = context.V(right_value);
    auto result = context.V(ir::Value{inst});
    const u32 control = inst->GetArg<ir::Imm>(2).Get() & 0xFFu;
    const bool same_source = left_value.Id() == right_value.Id();

    // One-instruction shapes. The 64-bit ZIPs select one complete qword from
    // each source; the 32-bit UZPs select even/odd dwords. EXT covers the
    // contiguous a[2..3],b[0..1] form.
    if (control == 0x44) {
        __ Zip1(result.V2D(), left.V2D(), right.V2D());
        return;
    }
    if (control == 0xEE) {
        __ Zip2(result.V2D(), left.V2D(), right.V2D());
        return;
    }
    if (control == 0x4E) {
        __ Ext(result.V16B(), left.V16B(), right.V16B(), 8);
        return;
    }
    if (control == 0x88) {
        __ Uzp1(result.V4S(), left.V4S(), right.V4S());
        return;
    }
    if (control == 0xDD) {
        __ Uzp2(result.V4S(), left.V4S(), right.V4S());
        return;
    }

    if (same_source) {
        // Alias-only shapes gain more one-instruction mappings because both
        // architectural operands name the same old destination.
        switch (control) {
            case 0xE4:
                if (sse_shufps_imm && result.GetCode() == left.GetCode()) {
                    ASSERT_MSG(ReproveShufpsImmTie(inst),
                               "SHUFPS imm tie proof diverged at IR {}", inst->Id());
                }
                if (result.GetCode() != left.GetCode()) {
                    __ Orr(result.V16B(), left.V16B(), left.V16B());
                }
                return;
            case 0x00: __ Dup(result.V4S(), left.V4S(), 0); return;
            case 0x55: __ Dup(result.V4S(), left.V4S(), 1); return;
            case 0xAA: __ Dup(result.V4S(), left.V4S(), 2); return;
            case 0xFF: __ Dup(result.V4S(), left.V4S(), 3); return;
            case 0x39: __ Ext(result.V16B(), left.V16B(), left.V16B(), 4); return;
            case 0x93: __ Ext(result.V16B(), left.V16B(), left.V16B(), 12); return;
            case 0x50: __ Zip1(result.V4S(), left.V4S(), left.V4S()); return;
            case 0xFA: __ Zip2(result.V4S(), left.V4S(), left.V4S()); return;
            case 0xA0: __ Trn1(result.V4S(), left.V4S(), left.V4S()); return;
            case 0xF5: __ Trn2(result.V4S(), left.V4S(), left.V4S()); return;
            // c-ray's two dominant splat-low shapes preserve the old high
            // qword. When RA ties result to source, one INS is sufficient.
            case 0xE0:
            case 0xE5: {
                if (sse_shufps_imm && result.GetCode() == left.GetCode()) {
                    ASSERT_MSG(ReproveShufpsImmTie(inst),
                               "SHUFPS imm tie proof diverged at IR {}", inst->Id());
                }
                if (result.GetCode() != left.GetCode()) {
                    __ Orr(result.V16B(), left.V16B(), left.V16B());
                }
                const u32 selected = control & 3;
                const u32 destination = selected == 0 ? 1 : 0;
                __ Ins(result.V4S(), destination, left.V4S(), selected);
                return;
            }
            default: break;
        }

        // A single table is sufficient for every remaining alias control.
        auto indexes = context.GetTmpV();
        auto tmp = context.GetTmpX();
        u64 index_lo = 0;
        u64 index_hi = 0;
        for (u32 byte = 0; byte < 16; ++byte) {
            const u32 lane = byte / 4;
            const u8 index = u8(((control >> (lane * 2)) & 3) * 4 + (byte & 3));
            auto& half = byte < 8 ? index_lo : index_hi;
            half |= u64(index) << ((byte & 7) * 8);
        }
        __ Mov(tmp, index_lo);
        __ Fmov(indexes.D(), tmp);
        __ Mov(tmp, index_hi);
        __ Ins(indexes.V2D(), 1, tmp);
        __ Tbl(result.V16B(), left.V16B(), indexes.V16B());
        return;
    }

    if (sse_shufps_imm && control == 0xe4 &&
        result.GetCode() == left.GetCode()) {
        ASSERT_MSG(ReproveShufpsImmTie(inst),
                   "SHUFPS imm tie proof diverged at IR {}", inst->Id());
        __ Ins(result.V2D(), 1, right.V2D(), 1);
        return;
    }

    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 lane = byte / 4;
        const u32 table_base = lane < 2 ? 0 : 16;
        const u8 index =
                u8(table_base + ((control >> (lane * 2)) & 3) * 4 + (byte & 3));
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }

    VRegister table0;
    VRegister table1;
    if (context.TryGetConsecutiveTmpV2(table0, table1)) {
        auto indexes = context.GetTmpV();
        auto tmp = context.GetTmpX();
        __ Orr(table0.V16B(), left.V16B(), left.V16B());
        __ Orr(table1.V16B(), right.V16B(), right.V16B());
        __ Mov(tmp, index_lo);
        __ Fmov(indexes.D(), tmp);
        __ Mov(tmp, index_hi);
        __ Ins(indexes.V2D(), 1, tmp);
        __ Tbl(result.V16B(), table0.V16B(), table1.V16B(), indexes.V16B());
        return;
    }

    // Extremely fragmented high-pressure fallback: two arbitrary scratch
    // registers are enough for TBL(left) followed by TBX(right). It has the
    // same 32-byte table semantics and still never leaves generated code.
    auto indexes = context.GetTmpV();
    auto sixteen = context.GetTmpV();
    auto tmp = context.GetTmpX();
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), left.V16B(), indexes.V16B());
    __ Movi(sixteen.V16B(), 16);
    __ Sub(indexes.V16B(), indexes.V16B(), sixteen.V16B());
    __ Tbx(result.V16B(), right.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecLoadConst(ir::Inst* inst) {
    auto result = context.V(ir::Value{inst});
    auto tmp = context.GetTmpX();
    const u64 low = inst->GetArg<ir::Imm>(0).Get();
    const u64 high = inst->GetArg<ir::Imm>(1).Get();
    // JitContext sizes the code-cache allocation before FinalizeCode() emits
    // VIXL's pending literal pool.  A literal LDR here therefore makes the
    // finalized unit larger than its allocation and truncates/corrupts the
    // pool.  Keep the block-local SSA cache, but materialize its one canonical
    // table with the established PIC-safe sequence until code-cache sizing can
    // account for deferred pools.
    __ Mov(tmp, low);
    __ Fmov(result.D(), tmp);
    __ Mov(tmp, high);
    __ Ins(result.V2D(), 1, tmp);
}

void JitTranslator::EmitVecShuffle32Indexed(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto indexes = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecSharedZero(ir::Inst* inst) {
    auto result = context.V(ir::Value{inst});
    __ Eor(result.V16B(), result.V16B(), result.V16B());
}

void JitTranslator::EmitVecShuffle16(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    auto tmp = context.GetTmpX();
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    const u32 base_lane = inst->GetArg<ir::Imm>(2).Get() ? 4 : 0;
    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 lane = byte / 2;
        u32 selected_lane = lane;
        if (lane >= base_lane && lane < base_lane + 4) {
            selected_lane = base_lane + ((control >> ((lane - base_lane) * 2)) & 3);
        }
        const u8 index = selected_lane * 2 + (byte & 1);
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecZip(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool high = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (high)
                __ Zip2(result.V16B(), left.V16B(), right.V16B());
            else
                __ Zip1(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (high)
                __ Zip2(result.V8H(), left.V8H(), right.V8H());
            else
                __ Zip1(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (high)
                __ Zip2(result.V4S(), left.V4S(), right.V4S());
            else
                __ Zip1(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            if (high)
                __ Zip2(result.V2D(), left.V2D(), right.V2D());
            else
                __ Zip1(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecDupPairs32(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (inst->GetArg<ir::Imm>(1).Get()) {
        __ Trn2(result.V4S(), src.V4S(), src.V4S());
    } else {
        __ Trn1(result.V4S(), src.V4S(), src.V4S());
    }
}

void JitTranslator::EmitVecDup64(ir::Inst* inst) {
    auto src = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    __ Dup(result.V2D(), src);
}

void JitTranslator::EmitVecExtract64(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 1;
    __ Umov(result, src.V2D(), lane);
}

void JitTranslator::EmitVecExtract16(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.W(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 7;
    __ Umov(result, src.V8H(), lane);
}

void JitTranslator::EmitVecInsert16(ir::Inst* inst) {
    auto dest = context.V(inst->GetArg<ir::Value>(0));
    auto value = context.W(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(2).Get() & 7;
    __ Orr(result.V16B(), dest.V16B(), dest.V16B());
    __ Ins(result.V8H(), lane, value);
}

void JitTranslator::EmitVecMovMask(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.W(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(1).Get();
    if (lane_bits == 8) {
        auto packed = context.GetTmpV();
        auto work = context.GetTmpV();
        __ Ushr(work.V16B(), src.V16B(), 7);
        __ Uzp1(packed.V16B(), work.V16B(), work.V16B());
        __ Uzp2(work.V16B(), work.V16B(), work.V16B());
        __ Sli(packed.V16B(), work.V16B(), 1);
        __ Uzp2(work.V16B(), packed.V16B(), packed.V16B());
        __ Uzp1(packed.V16B(), packed.V16B(), packed.V16B());
        __ Sli(packed.V16B(), work.V16B(), 2);
        __ Uzp2(work.V16B(), packed.V16B(), packed.V16B());
        __ Uzp1(packed.V16B(), packed.V16B(), packed.V16B());
        __ Sli(packed.V16B(), work.V16B(), 4);
        __ Umov(result, packed.V8H(), 0);
        return;
    }
    if (lane_bits == 32) {
        auto work = context.GetTmpV();
        auto shifts = context.GetTmpV();
        __ Ushr(work.V4S(), src.V4S(), 31);
        __ Movi(shifts.V16B(), 0x0000000300000002ull, 0x0000000100000000ull);
        __ Ushl(work.V4S(), work.V4S(), shifts.V4S());
        __ Addv(work.S(), work.V4S());
        __ Umov(result, work.V4S(), 0);
        return;
    }
    if (lane_bits == 64) {
        auto work = context.GetTmpV();
        auto packed = context.GetTmpX();
        __ Uzp2(work.V4S(), src.V4S(), src.V4S());
        __ Umov(packed, work.V2D(), 0);
        __ Bfi(packed, packed, 31, 32);
        __ Lsr(packed, packed, 62);
        __ Mov(result, packed.W());
        return;
    }
    PANIC("invalid vector movmask lane width");
}

void JitTranslator::EmitVecTableLookup8(ir::Inst* inst) {
    auto table = context.V(inst->GetArg<ir::Value>(0));
    auto control = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    __ Movi(indexes.V16B(), 0x8F);
    __ And(indexes.V16B(), control.V16B(), indexes.V16B());
    __ Tbl(result.V16B(), table.V16B(), indexes.V16B());
}

namespace {

// This VIXL snapshot has the AArch64 Crypto Extension encodings and feature
// decoder, but deliberately no assembler entry points for them.  Keep the
// encodings here, next to their IR lowerings, rather than adding a parallel
// assembler API to the vendored dependency.  All operands are Q registers.
constexpr u32 kAese = 0x4E284800;
constexpr u32 kAesd = 0x4E285800;
constexpr u32 kAesmc = 0x4E286800;
constexpr u32 kAesimc = 0x4E287800;
// PMULL.1Q Vd, Vn.1D, Vm.1D.  The 0x00c00000 size field is required for
// the 128-bit polynomial-product form; omitting it emits a different crypto
// encoding and corrupts even the low/low PCLMULQDQ case.
constexpr u32 kPmull = 0x0EE0E000;
constexpr u32 kPmull2 = kPmull | 0x40000000;
constexpr u32 kSha256H = 0x5E004000;
constexpr u32 kSha256H2 = 0x5E005000;
constexpr u32 kSha256Su0 = 0x5E282800;
constexpr u32 kSha256Su1 = 0x5E006000;

u32 Crypto2(u32 opcode, const VRegister& dst, const VRegister& src) {
    return opcode | (src.GetCode() << 5) | dst.GetCode();
}

u32 Crypto3(u32 opcode, const VRegister& dst, const VRegister& left, const VRegister& right) {
    return opcode | (right.GetCode() << 16) | (left.GetCode() << 5) | dst.GetCode();
}

}  // namespace

void JitTranslator::EmitVecAesEnc(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    // x86 AESENC = MC(SR(SB(data))) xor key.  ARM AESE has the key xor
    // before SB/SR, so feed it a zero key and add the x86 round key after
    // AESMC.  This is the established ARM/x86 round-order mapping.
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAese, result, zero));
    masm.dci(Crypto2(kAesmc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesEncLast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAese, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDec(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAesd, result, zero));
    masm.dci(Crypto2(kAesimc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDecLast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAesd, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesEncFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAese, result, zero));
    masm.dci(Crypto2(kAesmc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesEncLastFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAese, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDecFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAesd, result, zero));
    masm.dci(Crypto2(kAesimc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDecLastFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAesd, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesKeygenAssist(ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto sbox_shifted = context.GetTmpV();
    auto zero = context.GetTmpV();
    auto control = context.GetTmpV();
    auto rcon = context.GetTmpV();
    auto scratch = context.GetTmpX();
    const u64 rcon_byte = inst->GetArg<ir::Imm>(1).Get() & 0xff;

    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(sbox_shifted.V16B(), source.V16B(), source.V16B());
    masm.dci(Crypto2(kAese, sbox_shifted, zero));
    // AESE applies ShiftRows with its S-box.  This is FEX's established
    // AESKEYGENASSIST swizzle in host-little-endian byte order.  It produces
    // {SubWord(X1), RotWord(SubWord(X1)), SubWord(X3),
    //  RotWord(SubWord(X3))}; do not substitute a generic inverse-ShiftRows
    // mask here, because the required two dword pairs have different source
    // positions after AESE.
    __ Mov(scratch, 0x040B0E010B0E0104ULL);
    __ Fmov(control.D(), scratch);
    __ Mov(scratch, 0x0C0306090306090CULL);
    __ Ins(control.V2D(), 1, scratch);
    __ Tbl(result.V16B(), sbox_shifted.V16B(), control.V16B());
    // The x86 destination dwords are {SubWord(X1),
    // RotWord(SubWord(X1))^rcon, SubWord(X3),
    // RotWord(SubWord(X3))^rcon}; rcon affects byte zero of dwords 1 and 3.
    // In the little-endian vector representation those are byte offsets 4
    // and 12, not byte offsets 0 and 8.  FEX expresses the same placement as
    // (RCON << 32) duplicated across the two 64-bit lanes.
    __ Eor(rcon.V16B(), rcon.V16B(), rcon.V16B());
    __ Mov(scratch, rcon_byte << 32);
    __ Fmov(rcon.D(), scratch);
    __ Ins(rcon.V2D(), 1, scratch);
    __ Eor(result.V16B(), result.V16B(), rcon.V16B());
}

void JitTranslator::EmitVecPclMul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 select = inst->GetArg<ir::Imm>(2).Get();
    // PCLMULQDQ's high-high form maps exactly to ARM64 PMULL2.  Routing it
    // through two lane DUPs plus PMULL inflated the GHASH Karatsuba fold by
    // two host instructions per 0x11 multiply; FEX takes this PMULL2 form.
    if ((select & 0x11) == 0x11 && GcmPclMul2Enabled(context.GetFeatures())) {
        // The VIXL snapshot exposes Pmull2 but emits an unallocated sentinel
        // for it.  Keep this beside the existing raw PMULL encoding instead.
        masm.dci(Crypto3(kPmull2, result, left, right));
        return;
    }
    // PMULL consumes lane 0.  Duplicate a selected high 64-bit lane when the
    // x86 immediate asks for it; the actual multiplication remains one PMULL.
    if (select & 0x01) {
        auto tmp = context.GetTmpV();
        __ Dup(tmp.V2D(), left.V2D(), 1);
        left = tmp;
    }
    if (select & 0x10) {
        auto tmp = context.GetTmpV();
        __ Dup(tmp.V2D(), right.V2D(), 1);
        right = tmp;
    }
    masm.dci(Crypto3(kPmull, result, left, right));
}

void JitTranslator::EmitVecSha256Msg1(ir::Inst* inst) {
    auto destination = context.V(inst->GetArg<ir::Value>(0));
    auto source = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Orr(result.V16B(), destination.V16B(), destination.V16B());
    masm.dci(Crypto2(kSha256Su0, result, source));
}

void JitTranslator::EmitVecSha256Msg2(ir::Inst* inst) {
    auto destination = context.V(inst->GetArg<ir::Value>(0));
    auto source = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto src1 = context.GetTmpV();
    auto dup = context.GetTmpV();
    // FEX: src1 = ext(dst, dst, 3 dwords); src2 = zip2(dup(dst.d[3]), src).
    __ Ext(src1.V16B(), destination.V16B(), destination.V16B(), 12);
    __ Dup(dup.V4S(), destination.V4S(), 3);
    __ Zip2(dup.V2D(), dup.V2D(), source.V2D());
    __ Eor(result.V16B(), result.V16B(), result.V16B());
    masm.dci(Crypto3(kSha256Su1, result, src1, dup));
}

void JitTranslator::EmitVecSha256Rnds2(ir::Inst* inst) {
    auto destination = context.V(inst->GetArg<ir::Value>(0));
    auto source = context.V(inst->GetArg<ir::Value>(1));
    auto xmm0 = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    auto abcd = context.GetTmpV();
    auto efgh = context.GetTmpV();
    auto h2 = context.GetTmpV();
    auto key = context.GetTmpV();
    // FEX's x86-to-Arm SHA state mapping: Rev64(Zip2(src, dst)) is abcd,
    // Rev64(Zip1(src, dst)) is efgh.  SHA256RNDS2 reads only XMM0[63:0].
    __ Zip2(abcd.V2D(), source.V2D(), destination.V2D());
    __ Rev64(abcd.V4S(), abcd.V4S());
    __ Zip1(efgh.V2D(), source.V2D(), destination.V2D());
    __ Rev64(efgh.V4S(), efgh.V4S());
    __ Dup(key.V2D(), xmm0.V2D(), 0);
    __ Orr(h2.V16B(), efgh.V16B(), efgh.V16B());
    masm.dci(Crypto3(kSha256H2, h2, abcd, key));
    masm.dci(Crypto3(kSha256H, abcd, efgh, key));
    __ Zip2(result.V2D(), h2.V2D(), abcd.V2D());
    __ Rev64(result.V4S(), result.V4S());
}

VRegister JitTranslator::GetVecScalarOperand(ir::Value value, u32 lane_bits) {
    ASSERT(lane_bits == 32 || lane_bits == 64);
    if (ir::IsFloatValueType(value.Type())) {
        return context.V(value);
    }
    auto result = context.GetTmpV();
    if (lane_bits == 32) {
        __ Fmov(result.S(), context.W(value));
    } else {
        __ Fmov(result.D(), context.X(value));
    }
    return result;
}

bool JitTranslator::UseAFPNaN(ir::Inst* inst) const {
    if (!sse_afp_nan || !inst) {
        return false;
    }

    // P1 is deliberately an opcode-and-shape allowlist. Do not infer finite
    // values or admit a neighbouring FP opcode merely because AH happens to
    // improve its behaviour. Scalar binary opcodes encode their element type
    // in the opcode; packed binary and unary shapes carry it as an immediate.
    switch (inst->GetOp()) {
        case ir::OpCode::VecFAddScalar32:
        case ir::OpCode::VecFSubScalar32:
        case ir::OpCode::VecFMulScalar32:
        case ir::OpCode::VecFDivScalar32:
        case ir::OpCode::VecFAddScalar64:
        case ir::OpCode::VecFSubScalar64:
        case ir::OpCode::VecFMulScalar64:
        case ir::OpCode::VecFDivScalar64:
            return true;
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        case ir::OpCode::VecFDiv: {
            const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
            return lane_bits == 32 || lane_bits == 64;
        }
        case ir::OpCode::VecFUnary: {
            const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
            const u32 kind = inst->GetArg<ir::Imm>(3).Get();
            const u32 scalar = inst->GetArg<ir::Imm>(4).Get();
            return kind == 0 && (lane_bits == 32 || lane_bits == 64) &&
                   scalar <= 1;
        }
        default:
            // FMA, MIN/MAX, COMIS, RCP and RSQRT stay on their existing
            // correction/lowering paths until separately proven.
            return false;
    }
}

VRegister JitTranslator::PreserveNaNColdSource(ir::Inst* inst,
                                                const VRegister& source,
                                                const VRegister& result,
                                                const VRegister& reserved) {
    if (UseAFPNaN(inst)) {
        return source;
    }
    if (sse_nan_coldpath && !sse_nan_fast &&
        source.GetCode() == result.GetCode()) {
        __ Orr(reserved.V16B(), source.V16B(), source.V16B());
        return reserved;
    }
    return source;
}

void JitTranslator::QueueVecNaNColdPath(VecNaNColdKind kind,
                                        const VRegister& result,
                                        const VRegister& left,
                                        const VRegister& right) {
    ASSERT_MSG(!(context.GetFeatures().fpr_ipv_reclaim && sse_afp_nan),
               "AFP FPR reclamation reached the v11-v14 NaN cold ABI");
    auto site = std::make_unique<VecNaNColdSite>(
            VecNaNColdSite{kind, left, right, result});

    // For add/sub/mul/div, this one predicate is the exact OR of all reasons
    // the repair is needed:
    //
    //   isnan(result) ==
    //       isnan(left) || isnan(right) || operation_was_invalid
    //
    // FSQRT has the analogous identity with a NaN input or a negative finite
    // input. The host operation therefore performs both input and generated-
    // NaN detection for us, with a single conditional branch at the site.
    const bool scalar =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryScalar64 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtScalar64;
    const bool bits32 =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryPacked32 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtPacked32;

    if (scalar) {
        // FCMP owns NZCV. Commit a preceding lazy x86 flag producer before
        // using it as the cold predicate; the FP opcode itself defines no x86
        // integer flags.
        MergeNZCV();
        context.BeginHotNaNGuard(2);
        if (bits32) {
            __ Fcmp(result.S(), result.S());
        } else {
            __ Fcmp(result.D(), result.D());
        }
        __ B(site->slow.get(), vs);
        context.EndHotNaNGuard();
    } else if (bits32) {
        // FCMEQ produces all ones for every ordered lane. UMINV reduces that
        // to zero iff at least one lane is NaN, while leaving NZCV untouched.
        context.BeginHotNaNGuard(4);
        __ Fcmeq(ipv3.V4S(), result.V4S(), result.V4S());
        __ Uminv(ipv3.S(), ipv3.V4S());
        const auto ordered = context.GetSharedTmpX();
        __ Fmov(ordered.W(), ipv3.S());
        __ Cbz(ordered.W(), site->slow.get());
        context.EndHotNaNGuard();
    } else {
        context.BeginHotNaNGuard(5);
        __ Fcmeq(ipv3.V2D(), result.V2D(), result.V2D());
        const auto lane0 =
                backend::ScratchXPoolEnabled(context.GetFeatures()) ? context.GetTmpX() : ip0;
        const auto lane1 =
                backend::ScratchXPoolEnabled(context.GetFeatures()) ? context.GetTmpX() : ip1;
        __ Umov(lane0, ipv3.V2D(), 0);
        __ Umov(lane1, ipv3.V2D(), 1);
        __ And(lane0, lane0, lane1);
        __ Cbz(lane0, site->slow.get());
        context.EndHotNaNGuard();
    }

    __ Bind(site->continuation.get());
    vec_nan_cold_sites.push_back(std::move(site));
}

void JitTranslator::EmitVecNaNColdHandler(VecNaNColdKind kind) {
    const bool binary =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryScalar64 ||
            kind == VecNaNColdKind::BinaryPacked32 ||
            kind == VecNaNColdKind::BinaryPacked64;
    const bool scalar =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryScalar64 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtScalar64;
    const bool bits32 =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryPacked32 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtPacked32;
    const u32 lane_bits = bits32 ? 32 : 64;
    auto bytes = [scalar](const VRegister& v) {
        return scalar ? v.V8B() : v.V16B();
    };
    auto ordered = [&](const VRegister& d, const VRegister& s) {
        if (scalar) {
            if (bits32) {
                __ Fcmeq(d.S(), s.S(), s.S());
            } else {
                __ Fcmeq(d.D(), s.D(), s.D());
            }
        } else if (bits32) {
            __ Fcmeq(d.V4S(), s.V4S(), s.V4S());
        } else {
            __ Fcmeq(d.V2D(), s.V2D(), s.V2D());
        }
    };
    auto splat = [&](const VRegister& d, bool indefinite) {
        if (indefinite) {
            __ Mvni(d.V4S(), lane_bits == 32 ? 0x3F : 0x07, MSL, 16);
        } else {
            __ Movi(d.V4S(), lane_bits == 32 ? 0x00400000u : 0x00080000u);
        }
        if (lane_bits == 64) {
            __ Shl(d.V2D(), d.V2D(), 32);
        }
    };
    auto insert_scalar = [&](const VRegister& dst, const VRegister& src) {
        if (bits32) {
            __ Ins(dst.V4S(), 0, src.V4S(), 0);
        } else {
            __ Ins(dst.V2D(), 0, src.V2D(), 0);
        }
    };

    if (binary) {
        // Cold ABI:
        //   v11 = original operand 1
        //   v12 = original operand 2, then selected/quieted replacement
        //   v13 = hardware result, then repaired result
        //   v14 = ordered mask
        ordered(ipv3, ipv0);
        __ Bif(bytes(ipv1), bytes(ipv0), bytes(ipv3));
        ordered(ipv3, ipv1);
        if (bits32) {
            __ Orr(ipv1.V4S(), 0x40, 16);
        } else {
            splat(ipv0, false);
            __ Orr(bytes(ipv1), bytes(ipv1), bytes(ipv0));
        }
        splat(ipv0, true);
        __ Bit(bytes(ipv1), bytes(ipv0), bytes(ipv3));
        if (scalar) {
            insert_scalar(ipv2, ipv1);
        } else {
            ordered(ipv3, ipv2);
            __ Bif(bytes(ipv2), bytes(ipv1), bytes(ipv3));
        }
    } else {
        // Unary SQRT selects a quieted source NaN, or x86's negative
        // indefinite when an ordered (therefore negative finite) input made
        // FSQRT return NaN. Valid lanes retain the hardware result.
        ordered(ipv3, ipv0);
        if (bits32) {
            __ Orr(ipv0.V4S(), 0x40, 16);
        } else {
            splat(ipv1, false);
            __ Orr(bytes(ipv0), bytes(ipv0), bytes(ipv1));
        }
        splat(ipv1, true);
        __ Bit(bytes(ipv0), bytes(ipv1), bytes(ipv3));
        if (scalar) {
            insert_scalar(ipv2, ipv0);
        } else {
            ordered(ipv3, ipv2);
            __ Bif(bytes(ipv2), bytes(ipv0), bytes(ipv3));
        }
    }
    __ Br(atomic_pair_scratch);
}

void JitTranslator::EmitVecNaNColdPaths() {
    if (vec_nan_cold_sites.empty()) {
        return;
    }

    constexpr size_t kKindCount = 8;
    std::array<Label, kKindCount> handlers;
    std::array<bool, kKindCount> used{};
    auto index = [](VecNaNColdKind kind) {
        return static_cast<size_t>(kind);
    };
    auto is_binary = [](VecNaNColdKind kind) {
        return kind == VecNaNColdKind::BinaryScalar32 ||
               kind == VecNaNColdKind::BinaryScalar64 ||
               kind == VecNaNColdKind::BinaryPacked32 ||
               kind == VecNaNColdKind::BinaryPacked64;
    };

    // Normal execution has already terminated before this point. Each cold
    // veneer copies the site-specific physical registers into the fixed ABI,
    // calls one shared repair body, writes the repaired value back, and
    // resumes immediately after that site's test.
    for (auto& site : vec_nan_cold_sites) {
        const auto i = index(site->kind);
        used[i] = true;
        __ Bind(site->slow.get());
        if (site->left.GetCode() != ipv0.GetCode()) {
            __ Orr(ipv0.V16B(), site->left.V16B(), site->left.V16B());
        }
        if (is_binary(site->kind) && site->right.GetCode() != ipv1.GetCode()) {
            __ Orr(ipv1.V16B(), site->right.V16B(), site->right.V16B());
        }
        __ Orr(ipv2.V16B(), site->result.V16B(), site->result.V16B());
        __ Adr(atomic_pair_scratch, site->repaired.get());
        __ B(&handlers[i]);
        __ Bind(site->repaired.get());
        __ Orr(site->result.V16B(), ipv2.V16B(), ipv2.V16B());
        __ B(site->continuation.get());
    }

    for (size_t i = 0; i < kKindCount; ++i) {
        if (!used[i]) {
            continue;
        }
        __ Bind(&handlers[i]);
        EmitVecNaNColdHandler(static_cast<VecNaNColdKind>(i));
    }
    vec_nan_cold_sites.clear();
}

void JitTranslator::EmitVecFAddScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fadd(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFSubScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fsub(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFMulScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fmul(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFDivScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fdiv(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFAddScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fadd(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFSubScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fsub(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFMulScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fmul(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFDivScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fdiv(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFScalarBinaryTied(ir::Inst* inst, u32 lane_bits) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto right_value = inst->GetArg<ir::Value>(1);
    const bool right_is_vector = ir::IsFloatValueType(right_value.Type());
    auto right = right_is_vector ? context.V(right_value) : context.GetTmpV();
    if (result.GetCode() == left.GetCode()) {
        auto left_value = inst->GetArg<ir::Value>(0);
        while (left_value.Defined() && left_value.Def()->IsBitCastOperation()) {
            left_value = left_value.Def()->GetArg<ir::Value>(0);
        }
        if (sse_scalar_tie && left_value.Defined() && left_value.Def() &&
            left_value.Def()->GetOp() == ir::OpCode::GetHostFPR &&
            left_value.Def()->GetArg<ir::Imm>(0).Get() >= 16 &&
            context.IsHostReadCoalesced(left_value.Id())) {
            ASSERT_MSG(ReproveScalarFPRTie(inst),
                       "scalar FPR fixed-home tie proof diverged at IR {}", inst->Id());
        }
    } else {
        // A still-live dst-in cannot be tied by RA. Seed the new destination
        // once, then let NEP update lane 0 in place; this is still one copy
        // instead of the legacy post-op full-copy plus lane insert.
        __ Orr(result.V16B(), left.V16B(), left.V16B());
    }
    if (!right_is_vector) {
        if (lane_bits == 32) {
            __ Fmov(right.S(), context.W(right_value));
        } else {
            __ Fmov(right.D(), context.X(right_value));
        }
    }
    auto emit_operation = [&] {
        switch (inst->GetOp()) {
            case ir::OpCode::VecFAddScalar32:
                __ Fadd(result.S(), left.S(), right.S());
                break;
            case ir::OpCode::VecFSubScalar32:
                __ Fsub(result.S(), left.S(), right.S());
                break;
            case ir::OpCode::VecFMulScalar32:
                __ Fmul(result.S(), left.S(), right.S());
                break;
            case ir::OpCode::VecFDivScalar32:
                __ Fdiv(result.S(), left.S(), right.S());
                break;
            case ir::OpCode::VecFAddScalar64:
                __ Fadd(result.D(), left.D(), right.D());
                break;
            case ir::OpCode::VecFSubScalar64:
                __ Fsub(result.D(), left.D(), right.D());
                break;
            case ir::OpCode::VecFMulScalar64:
                __ Fmul(result.D(), left.D(), right.D());
                break;
            case ir::OpCode::VecFDivScalar64:
                __ Fdiv(result.D(), left.D(), right.D());
                break;
            default:
                PANIC();
        }
    };
    if (UseAFPNaN(inst) || sse_nan_fast) {
        emit_operation();
        return;
    }
    if (sse_nan_coldpath) {
        auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
        auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
        emit_operation();
        EmitVecFloatNaNFixup(result,
                             repair_left,
                             repair_right,
                             lane_bits,
                             1,
                             inst);
        return;
    }

    // Build the exact x86 NaN result before a tied operation overwrites
    // `left`. All bitwise work targets temporaries; only the rare NaN result
    // takes the lane insert below, so ordinary arithmetic has no merge path.
    auto ordered = context.GetTmpV();
    auto propagated = context.GetTmpV();
    auto tmp = context.GetTmpV();
    auto ordered_scalar = [&](const VRegister& d, const VRegister& s) {
        if (lane_bits == 32) {
            __ Fcmeq(d.S(), s.S(), s.S());
        } else {
            __ Fcmeq(d.D(), s.D(), s.D());
        }
    };
    auto bytes = [](const VRegister& v) { return v.V8B(); };
    auto splat = [&](const VRegister& d, bool indefinite) {
        if (indefinite) {
            __ Mvni(d.V4S(), lane_bits == 32 ? 0x3F : 0x07, MSL, 16);
        } else {
            __ Movi(d.V4S(), lane_bits == 32 ? 0x00400000u : 0x00080000u);
        }
        if (lane_bits == 64) {
            __ Shl(d.V2D(), d.V2D(), 32);
        }
    };

    ordered_scalar(ordered, left);
    __ Orr(bytes(propagated), bytes(right), bytes(right));
    __ Bif(bytes(propagated), bytes(left), bytes(ordered));
    ordered_scalar(ordered, propagated);
    if (lane_bits == 32) {
        __ Orr(propagated.V4S(), 0x40, 16);
    } else {
        splat(tmp, false);
        __ Orr(bytes(propagated), bytes(propagated), bytes(tmp));
    }
    splat(tmp, true);
    __ Bit(bytes(propagated), bytes(tmp), bytes(ordered));

    emit_operation();
    Label done;
    if (lane_bits == 32) {
        __ Fcmp(result.S(), result.S());
        __ B(&done, vc);
        __ Ins(result.V4S(), 0, propagated.V4S(), 0);
    } else {
        __ Fcmp(result.D(), result.D());
        __ B(&done, vc);
        __ Ins(result.V2D(), 0, propagated.V2D(), 0);
    }
    __ Bind(&done);
}

void JitTranslator::EmitVecFloatNaNFixup(const VRegister& result,
                                         const VRegister& left,
                                         const VRegister& right,
                                         u32 lane_bits,
                                         u32 lane_count,
                                         ir::Inst* inst) {
    // Explicitly opt-in semantic relaxation. The immediately preceding NEON
    // arithmetic instruction becomes the final lane result; no operand-order
    // payload selection or x86 indefinite-NaN substitution is emitted.
    if (UseAFPNaN(inst) || sse_nan_fast) {
        return;
    }
    ASSERT(lane_bits == 32 || lane_bits == 64);
    if (sse_nan_coldpath) {
        QueueVecNaNColdPath(
                lane_count == 1
                        ? (lane_bits == 32
                                   ? VecNaNColdKind::BinaryScalar32
                                   : VecNaNColdKind::BinaryScalar64)
                        : (lane_bits == 32
                                   ? VecNaNColdKind::BinaryPacked32
                                   : VecNaNColdKind::BinaryPacked64),
                result,
                left,
                right);
        return;
    }
    // The scalar forms (lane_count == 1) define lane 0 only; their callers
    // merge the surviving lanes from `left` after this returns. Keeping the
    // bitwise ops 64-bit wide there is not an optimisation but a correctness
    // requirement for the 64-bit scalar shapes: writing the upper half of
    // `result` would destroy the very lane the caller is about to copy back
    // whenever RegAlloc gives `result` and `left` the same physical register.
    const bool scalar = lane_count == 1;
    auto bytes = [scalar](const VRegister& v) { return scalar ? v.V8B() : v.V16B(); };
    // isnan(x) == !(x == x). FCMEQ yields all-ones lanes where the compare
    // holds, so every mask below is the *complement* of "is NaN"; BIF/BIT read
    // that polarity directly and no NOT is needed anywhere.
    auto ordered = [&](const VRegister& d, const VRegister& s) {
        if (scalar) {
            if (lane_bits == 32) {
                __ Fcmeq(d.S(), s.S(), s.S());
            } else {
                __ Fcmeq(d.D(), s.D(), s.D());
            }
        } else {
            if (lane_bits == 32) {
                __ Fcmeq(d.V4S(), s.V4S(), s.V4S());
            } else {
                __ Fcmeq(d.V2D(), s.V2D(), s.V2D());
            }
        }
    };
    // Both constants are built with NEON modified immediates (one or two
    // instructions, no GPR): MOVI/MVNI cannot encode the 64-bit patterns
    // directly, but each is its 32-bit half shifted into place. Going through
    // MacroAssembler::Movi(vd, u64) instead would spill the value through
    // VIXL's scratch pool -- contract-leased, so correct, but a GPR round trip
    // and three instructions where these are two.
    auto splat = [&](const VRegister& d, bool indefinite) {
        if (indefinite) {
            // MSL: ~((imm8 << 16) | 0xFFFF).
            __ Mvni(d.V4S(), lane_bits == 32 ? 0x3F : 0x07, MSL, 16);
        } else {
            __ Movi(d.V4S(), lane_bits == 32 ? 0x00400000u : 0x00080000u);
        }
        if (lane_bits == 64) {
            __ Shl(d.V2D(), d.V2D(), 32);
        }
    };

    auto ordered_mask = context.GetTmpV();
    auto propagated = context.GetTmpV();
    auto tmp = context.GetTmpV();

    // x86 propagates operand 1's NaN in preference to operand 2's; ARM ranks
    // signalling NaNs above quiet ones instead, so the choice has to be redone
    // from the inputs rather than read off the hardware result.
    ordered(ordered_mask, left);
    __ Orr(bytes(propagated), bytes(right), bytes(right));
    __ Bif(bytes(propagated), bytes(left), bytes(ordered_mask));
    // `propagated` is a NaN exactly when one of the two inputs was (it *is*
    // one of them), so re-testing it costs one register less than keeping the
    // left mask alive and ANDing in a second one.
    ordered(ordered_mask, propagated);

    // Force the quiet bit on: a signalling input is forwarded as the matching
    // quiet NaN with its payload intact. The 32-bit bit pattern is an ORR
    // vector immediate; the 64-bit one is not encodable as one (the same
    // immediate would land in both halves of every lane and corrupt the
    // payload), so it goes through a register.
    if (lane_bits == 32) {
        __ Orr(propagated.V4S(), 0x40, 16);
    } else {
        splat(tmp, false);
        __ Orr(bytes(propagated), bytes(propagated), bytes(tmp));
    }

    // Lanes where the operation itself was invalid (inf-inf, 0*inf, 0/0, and
    // nothing else once neither input is a NaN) get x86's "real indefinite"
    // QNaN, which differs from ARM's default NaN in the sign bit.
    splat(tmp, true);
    __ Bit(bytes(propagated), bytes(tmp), bytes(ordered_mask));

    // Add/sub/mul/div return a NaN for *every* NaN input, so "the result is a
    // NaN" is exactly the set of lanes that needs one of the two substitutions
    // above; lanes that produced a number are already correct.
    ordered(ordered_mask, result);
    __ Bif(bytes(result), bytes(propagated), bytes(ordered_mask));
}

void JitTranslator::EmitVecFAdd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fadd(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fadd(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFSub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fsub(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fsub(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFMul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fmul(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fmul(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFDiv(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fdiv(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fdiv(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFMinMax(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool maximum = inst->GetArg<ir::Imm>(3).Get() != 0;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    if (sse_afp_minmax && scalar && result.GetCode() == left.GetCode()) {
        // AH selects operand 2 for unordered and equal inputs, matching x86
        // MIN/MAX including NaN payload and signed-zero selection.  NEP keeps
        // the tied destination's upper lanes intact.
        if (bits == 32) {
            if (maximum)
                __ Fmax(result.S(), left.S(), right.S());
            else
                __ Fmin(result.S(), left.S(), right.S());
        } else {
            if (maximum)
                __ Fmax(result.D(), left.D(), right.D());
            else
                __ Fmin(result.D(), left.D(), right.D());
        }
        return;
    }
    if (sse_scalar_insert && scalar) {
        // x86 selects operand 2 for unordered and equal. Keep dst-in as the
        // default, and insert operand 2 only when it wins.
        if (result.GetCode() != left.GetCode()) {
            __ Orr(result.V16B(), left.V16B(), left.V16B());
        }
        Label keep_left;
        if (bits == 32) {
            __ Fcmp(left.S(), right.S());
            __ B(&keep_left, maximum ? gt : mi);
            __ Ins(result.V4S(), 0, right.V4S(), 0);
        } else {
            __ Fcmp(left.D(), right.D());
            __ B(&keep_left, maximum ? gt : mi);
            __ Ins(result.V2D(), 0, right.V2D(), 0);
        }
        __ Bind(&keep_left);
        return;
    }
    auto mask = context.GetTmpV();
    auto selected = context.GetTmpV();
    if (bits == 32) {
        if (maximum)
            __ Fcmgt(mask.V4S(), left.V4S(), right.V4S());
        else
            __ Fcmgt(mask.V4S(), right.V4S(), left.V4S());
    } else {
        if (maximum)
            __ Fcmgt(mask.V2D(), left.V2D(), right.V2D());
        else
            __ Fcmgt(mask.V2D(), right.V2D(), left.V2D());
    }
    __ Bsl(mask.V16B(), left.V16B(), right.V16B());
    __ Orr(selected.V16B(), mask.V16B(), mask.V16B());
    if (!scalar) {
        __ Orr(result.V16B(), selected.V16B(), selected.V16B());
    } else {
        __ Orr(result.V16B(), left.V16B(), left.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, selected.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, selected.V2D(), 0);
    }
}

void JitTranslator::EmitVecFUnary(ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto merge = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 kind = inst->GetArg<ir::Imm>(3).Get();
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    if (sse_scalar_insert && scalar &&
        (result.GetCode() == merge.GetCode() || source.GetCode() != result.GetCode()) &&
        (kind == 0 || source.GetCode() != result.GetCode())) {
        if (result.GetCode() != merge.GetCode()) {
            __ Orr(result.V16B(), merge.V16B(), merge.V16B());
        }
        if (kind == 0) {
            if (UseAFPNaN(inst)) {
                if (bits == 32) {
                    __ Fsqrt(result.S(), source.S());
                } else {
                    __ Fsqrt(result.D(), source.D());
                }
                return;
            }
            if (sse_nan_coldpath) {
                auto repair_source = source;
                if (source.GetCode() == result.GetCode()) {
                    repair_source = ipv0;
                    __ Orr(repair_source.V16B(), source.V16B(), source.V16B());
                }
                if (bits == 32) {
                    __ Fsqrt(result.S(), source.S());
                    QueueVecNaNColdPath(VecNaNColdKind::SqrtScalar32,
                                        result,
                                        repair_source);
                } else {
                    __ Fsqrt(result.D(), source.D());
                    QueueVecNaNColdPath(VecNaNColdKind::SqrtScalar64,
                                        result,
                                        repair_source);
                }
                return;
            }
            // Compare before FSQRT because source may be the tied result.
            // Negative finite inputs select x86's signed indefinite; NaN and
            // -0.0 retain the hardware result.
            if (bits == 32) {
                __ Fcmp(source.S(), 0.0);
                __ Fsqrt(result.S(), source.S());
                auto indef = context.GetTmpV();
                auto imm = context.GetTmpX();
                __ Mov(imm.W(), 0xFFC00000u);
                __ Fmov(indef.S(), imm.W());
                Label done;
                __ B(&done, pl);
                __ Ins(result.V4S(), 0, indef.V4S(), 0);
                __ Bind(&done);
            } else {
                __ Fcmp(source.D(), 0.0);
                __ Fsqrt(result.D(), source.D());
                auto indef = context.GetTmpV();
                auto imm = context.GetTmpX();
                __ Mov(imm, UINT64_C(0xFFF8000000000000));
                __ Fmov(indef.D(), imm);
                Label done;
                __ B(&done, pl);
                __ Ins(result.V2D(), 0, indef.V2D(), 0);
                __ Bind(&done);
            }
        } else if (kind == 1) {
            __ Frecpe(result.S(), source.S());
            auto step = context.GetTmpV();
            for (u32 i = 0; i < 2; ++i) {
                __ Frecps(step.S(), source.S(), result.S());
                __ Fmul(result.S(), result.S(), step.S());
            }
        } else {
            __ Frsqrte(result.S(), source.S());
            auto square = context.GetTmpV();
            auto step = context.GetTmpV();
            for (u32 i = 0; i < 2; ++i) {
                __ Fmul(square.S(), result.S(), result.S());
                __ Frsqrts(step.S(), source.S(), square.S());
                __ Fmul(result.S(), result.S(), step.S());
            }
        }
        return;
    }
    auto value = context.GetTmpV();
    // x86 SQRT of a negative operand raises #I and delivers the QNaN
    // *indefinite*, whose sign bit is SET (0xFFC00000 / 0xFFF8000000000000).
    // ARM's FSQRT delivers the default NaN, whose sign is clear, so the two
    // disagree on exactly those lanes — verified against real x86 via Rosetta:
    // sqrtps(-4) = ffc00000, sqrtpd(-4) = fff8000000000000.
    //
    // FCMLT against zero is false for both NaN and -0.0, which is precisely the
    // wanted predicate: a NaN operand must keep propagating through FSQRT, and
    // sqrt(-0.0) is legitimately -0.0 on both architectures.
    //
    // This is not AVX-specific: legacy SSE sqrtps/sqrtpd/sqrtss/sqrtsd reach
    // the same IR opcode and were wrong in the same way.
    if (bits == 32) {
        if (kind == 0) {
            if (UseAFPNaN(inst)) {
                __ Fsqrt(value.V4S(), source.V4S());
            } else if (sse_nan_coldpath) {
                auto repair_source = source;
                if (source.GetCode() == value.GetCode()) {
                    repair_source = ipv0;
                    __ Orr(repair_source.V16B(), source.V16B(), source.V16B());
                }
                __ Fsqrt(value.V4S(), source.V4S());
                QueueVecNaNColdPath(
                        scalar ? VecNaNColdKind::SqrtScalar32
                               : VecNaNColdKind::SqrtPacked32,
                        value,
                        repair_source);
            } else {
                __ Fsqrt(value.V4S(), source.V4S());
                auto negative = context.GetTmpV();
                auto indef = context.GetTmpV();
                auto imm = context.GetTmpX();
                __ Fcmlt(negative.V4S(), source.V4S(), 0.0);
                __ Mov(imm.W(), 0xFFC00000u);
                __ Dup(indef.V4S(), imm.W());
                __ Bit(value.V16B(), indef.V16B(), negative.V16B());
            }
        } else if (kind == 1) {
            __ Frecpe(value.V4S(), source.V4S());
            auto step = context.GetTmpV();
            // FRECPE alone is less accurate than x86 RCPPS/RCPSS require.
            // Two Newton-Raphson refinements comfortably satisfy the x86
            // relative-error bound while retaining the estimate semantics.
            for (u32 i = 0; i < 2; ++i) {
                __ Frecps(step.V4S(), source.V4S(), value.V4S());
                __ Fmul(value.V4S(), value.V4S(), step.V4S());
            }
        } else {
            __ Frsqrte(value.V4S(), source.V4S());
            auto square = context.GetTmpV();
            auto step = context.GetTmpV();
            for (u32 i = 0; i < 2; ++i) {
                __ Fmul(square.V4S(), value.V4S(), value.V4S());
                __ Frsqrts(step.V4S(), source.V4S(), square.V4S());
                __ Fmul(value.V4S(), value.V4S(), step.V4S());
            }
        }
    } else {
        if (UseAFPNaN(inst)) {
            __ Fsqrt(value.V2D(), source.V2D());
        } else if (sse_nan_coldpath) {
            auto repair_source = source;
            if (source.GetCode() == value.GetCode()) {
                repair_source = ipv0;
                __ Orr(repair_source.V16B(), source.V16B(), source.V16B());
            }
            __ Fsqrt(value.V2D(), source.V2D());
            QueueVecNaNColdPath(
                    scalar ? VecNaNColdKind::SqrtScalar64
                           : VecNaNColdKind::SqrtPacked64,
                    value,
                    repair_source);
        } else {
            __ Fsqrt(value.V2D(), source.V2D());
            auto negative = context.GetTmpV();
            auto indef = context.GetTmpV();
            auto imm = context.GetTmpX();
            __ Fcmlt(negative.V2D(), source.V2D(), 0.0);
            __ Mov(imm, UINT64_C(0xFFF8000000000000));
            __ Dup(indef.V2D(), imm);
            __ Bit(value.V16B(), indef.V16B(), negative.V16B());
        }
    }
    if (!scalar) {
        __ Orr(result.V16B(), value.V16B(), value.V16B());
    } else {
        __ Orr(result.V16B(), merge.V16B(), merge.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, value.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, value.V2D(), 0);
    }
}

void JitTranslator::EmitVecFCmp(ir::Inst* inst) {
    auto left = GetVecScalarOperand(inst->GetArg<ir::Value>(0),
                                    inst->GetArg<ir::Imm>(2).Get());
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1),
                                     inst->GetArg<ir::Imm>(2).Get());
    auto result = context.X(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool compact = inst->GetArg<ir::Imm>(3).Get() != 0;
    // FCMP overwrites host NZCV.  Guest decoding normally puts an AdvancePC
    // boundary immediately before us; keep the IR opcode safe on its own too.
    MergeNZCV();
    FlushFlags();
    if (bits == 32) {
        __ Fcmp(left.S(), right.S());
    } else {
        __ Fcmp(left.D(), right.D());
    }

    if (compact) {
        // Preserve the one relation AXFLAG discards.  VC is ordered, which is
        // also the raw parity byte representation: 1 has odd parity (PF=0),
        // while unordered produces 0 (PF=1).
        __ Cset(result, vc);
        return;
    }

    // ARM FPCompare NZCV: less=N, equal=Z, greater=C, unordered=C|V.
    // x86 UCOMIS flags are CF=less|unordered, PF=unordered,
    // ZF=equal|unordered.
    auto bit = context.GetTmpX();
    __ Cset(bit, lt);
    __ Mov(result, bit);
    __ Cset(bit, vs);
    __ Orr(result, result, bit);
    __ Lsl(bit, bit, 1);
    __ Orr(result, result, bit);
    __ Cset(bit, eq);
    auto unordered = context.GetTmpX();
    __ Cset(unordered, vs);
    __ Orr(bit, bit, unordered);
    __ Lsl(bit, bit, 2);
    __ Orr(result, result, bit);
}

// The predicate is a relation set -- bit 0 = less, 1 = equal, 2 = greater,
// 3 = unordered; see the comment on VecFCmpMask in ir/ir.inc.
//
// All 16 sets are reachable, but only eight sequences are needed: a set and
// its complement give complementary masks, so `m` in 8..15 is emitted as the
// sequence for `15 - m` (which is always in 0..7) followed by one MVN.  That
// pairing is what makes the table below total by construction rather than by
// sixteen hand-checked cases -- 15-8=7 (ordered -> unordered), 15-9=6
// (ge -> !ge), 15-11=4 (gt -> !gt), 15-15=0 (never -> always), and so on.
//
// AArch64 FCMEQ/FCMGT/FCMGE are ORDERED: they give false for a NaN operand,
// which is what x86's ordered predicates want.  `unordered` has no direct
// instruction and is built from FCMEQ(x, x), false exactly for a NaN.  These
// primitives may raise FPSR.IOC where x86 would not; SwiftVM propagates no FP
// exception state either way, which is the same reason the front end drops
// AVX's signalling-vs-quiet dimension (frontend/x86/fp_cmp_predicate.h).
void JitTranslator::EmitVecFCmpMask(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 relations = inst->GetArg<ir::Imm>(3).Get() & 15;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    const bool invert = relations >= 8;
    const u32 predicate = invert ? 15 - relations : relations;
    auto compare = context.GetTmpV();
    auto ordered_left = context.GetTmpV();
    auto ordered_right = context.GetTmpV();
    auto format = [bits](const VRegister& value) {
        return bits == 32 ? value.V4S() : value.V2D();
    };
    switch (predicate) {
        case 0:  // {} -- never; with invert, {<,==,>,unord} -- always.
            __ Movi(compare.V16B(), 0);
            break;
        case 1:  // {<}
            __ Fcmgt(format(compare), format(right), format(left));
            break;
        case 2:  // {==}
            __ Fcmeq(format(compare), format(left), format(right));
            break;
        case 3:  // {<, ==}
            __ Fcmge(format(compare), format(right), format(left));
            break;
        case 4:  // {>}
            __ Fcmgt(format(compare), format(left), format(right));
            break;
        case 5:  // {<, >} -- ordered-and-not-equal.  Two ordered compares are
                 // cheaper than "ordered AND not equal", which needs three.
            __ Fcmgt(format(ordered_left), format(right), format(left));
            __ Fcmgt(format(ordered_right), format(left), format(right));
            __ Orr(compare.V16B(), ordered_left.V16B(), ordered_right.V16B());
            break;
        case 6:  // {==, >}
            __ Fcmge(format(compare), format(left), format(right));
            break;
        case 7:  // {<, ==, >} -- ordered; with invert, unordered.
            __ Fcmeq(format(ordered_left), format(left), format(left));
            __ Fcmeq(format(ordered_right), format(right), format(right));
            __ And(compare.V16B(), ordered_left.V16B(), ordered_right.V16B());
            break;
    }
    if (invert) {
        __ Mvn(compare.V16B(), compare.V16B());
    }
    if (!scalar) {
        __ Orr(result.V16B(), compare.V16B(), compare.V16B());
    } else {
        __ Orr(result.V16B(), left.V16B(), left.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, compare.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, compare.V2D(), 0);
    }
}

void JitTranslator::EmitVecFCvtIntToFloat(ir::Inst* inst) {
    auto source = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    auto fp = context.GetTmpV();
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    if (dst_bits == 32) {
        if (src_bits == 32)
            __ Scvtf(fp.S(), source.W());
        else
            __ Scvtf(fp.S(), source);
        __ Fmov(result.W(), fp.S());
    } else {
        if (src_bits == 32)
            __ Scvtf(fp.D(), source.W());
        else
            __ Scvtf(fp.D(), source);
        __ Fmov(result, fp.D());
    }
}

void JitTranslator::EmitVecFCvtFloatToInt(ir::Inst* inst) {
    auto source = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    auto fp = context.GetTmpV();
    auto bound = context.GetTmpV();
    auto bound_bits = context.GetTmpX();
    auto converted = context.GetTmpX();
    auto invalid = context.GetTmpX();
    auto test = context.GetTmpX();
    auto indefinite = context.GetTmpX();
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool round_nearest = inst->GetArg<ir::Imm>(3).Get() != 0;

    if (src_bits == 32) {
        __ Fmov(fp.S(), source.W());
    } else {
        __ Fmov(fp.D(), source);
    }
    if (dst_bits == 32) {
        if (src_bits == 32) {
            if (round_nearest) __ Fcvtns(converted.W(), fp.S());
            else __ Fcvtzs(converted.W(), fp.S());
        } else {
            if (round_nearest) __ Fcvtns(converted.W(), fp.D());
            else __ Fcvtzs(converted.W(), fp.D());
        }
    } else {
        if (src_bits == 32) {
            if (round_nearest) __ Fcvtns(converted, fp.S());
            else __ Fcvtzs(converted, fp.S());
        } else {
            if (round_nearest) __ Fcvtns(converted, fp.D());
            else __ Fcvtzs(converted, fp.D());
        }
    }

    // Detect NaN first (FCMP x,x => V set only for unordered), then the two
    // half-open integer bounds.  The lower endpoint is valid; the upper
    // endpoint is invalid because truncation cannot represent 2^N.
    if (src_bits == 32)
        __ Fcmp(fp.S(), fp.S());
    else
        __ Fcmp(fp.D(), fp.D());
    __ Cset(invalid, vs);

    const u64 upper_bits = src_bits == 32 ? (dst_bits == 32 ? 0x4F000000u : 0x5F000000u)
                           : (dst_bits == 32 ? 0x41E0000000000000ull
                                             : 0x43E0000000000000ull);
    const u64 lower_bits = src_bits == 32 ? (dst_bits == 32 ? 0xCF000000u : 0xDF000000u)
                           : (dst_bits == 32 ? 0xC1E0000000000000ull
                                             : 0xC3E0000000000000ull);
    __ Mov(bound_bits, upper_bits);
    if (src_bits == 32)
        __ Fmov(bound.S(), bound_bits.W());
    else
        __ Fmov(bound.D(), bound_bits);
    if (src_bits == 32)
        __ Fcmp(fp.S(), bound.S());
    else
        __ Fcmp(fp.D(), bound.D());
    __ Cset(test, hs);
    __ Orr(invalid, invalid, test);

    __ Mov(bound_bits, lower_bits);
    if (src_bits == 32)
        __ Fmov(bound.S(), bound_bits.W());
    else
        __ Fmov(bound.D(), bound_bits);
    if (src_bits == 32)
        __ Fcmp(fp.S(), bound.S());
    else
        __ Fcmp(fp.D(), bound.D());
    __ Cset(test, lt);
    __ Orr(invalid, invalid, test);

    __ Mov(indefinite, dst_bits == 32 ? 0x80000000u : 0x8000000000000000ull);
    __ Cmp(invalid, 0);
    if (dst_bits == 32)
        __ Csel(result.W(), indefinite.W(), converted.W(), ne);
    else
        __ Csel(result, indefinite, converted, ne);
}

void JitTranslator::EmitVecFCvtScalar(ir::Inst* inst) {
    auto source = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    auto fp = context.GetTmpV();
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    if (src_bits == 32) {
        __ Fmov(fp.S(), source.W());
        __ Fcvt(fp.D(), fp.S());
        __ Fmov(result, fp.D());
    } else {
        __ Fmov(fp.D(), source);
        __ Fcvt(fp.S(), fp.D());
        __ Fmov(result.W(), fp.S());
    }
}

void JitTranslator::EmitVecFCvtPacked(ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto wide = context.GetTmpV();
    const u32 kind = inst->GetArg<ir::Imm>(1).Get();
    switch (kind) {
        case 0: __ Scvtf(result.V4S(), source.V4S()); break;
        case 1:
            __ Sshll(wide.V2D(), source.V2S(), 0);
            __ Scvtf(result.V2D(), wide.V2D());
            break;
        case 2:
        case 3: {
            if (kind == 2)
                __ Fcvtns(result.V4S(), source.V4S());
            else
                __ Fcvtzs(result.V4S(), source.V4S());
            auto invalid = context.GetTmpV();
            auto compare = context.GetTmpV();
            auto bound = context.GetTmpV();
            auto bits = context.GetTmpX();
            auto indefinite = context.GetTmpV();
            __ Fcmeq(invalid.V4S(), source.V4S(), source.V4S());
            __ Mvn(invalid.V16B(), invalid.V16B());
            __ Mov(bits.W(), 0x4F000000u);  // +2^31
            __ Dup(bound.V4S(), bits.W());
            __ Fcmge(compare.V4S(), source.V4S(), bound.V4S());
            __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
            __ Mov(bits.W(), 0xCF000000u);  // -2^31 (valid endpoint)
            __ Dup(bound.V4S(), bits.W());
            __ Fcmgt(compare.V4S(), bound.V4S(), source.V4S());
            __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
            __ Mov(bits.W(), 0x80000000u);
            __ Dup(indefinite.V4S(), bits.W());
            __ Bsl(invalid.V16B(), indefinite.V16B(), result.V16B());
            __ Orr(result.V16B(), invalid.V16B(), invalid.V16B());
            break;
        }
        case 4:
        case 5:
            if (kind == 4)
                __ Fcvtns(wide.V2D(), source.V2D());
            else
                __ Fcvtzs(wide.V2D(), source.V2D());
            {
                auto invalid = context.GetTmpV();
                auto compare = context.GetTmpV();
                auto bound = context.GetTmpV();
                auto bits = context.GetTmpX();
                auto indefinite = context.GetTmpV();
                __ Fcmeq(invalid.V2D(), source.V2D(), source.V2D());
                __ Mvn(invalid.V16B(), invalid.V16B());
                __ Mov(bits, 0x41E0000000000000ull);  // +2^31
                __ Dup(bound.V2D(), bits);
                __ Fcmge(compare.V2D(), source.V2D(), bound.V2D());
                __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
                __ Mov(bits, 0xC1E0000000000000ull);  // -2^31
                __ Dup(bound.V2D(), bits);
                __ Fcmgt(compare.V2D(), bound.V2D(), source.V2D());
                __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
                __ Mov(bits, 0x80000000u);
                __ Dup(indefinite.V2D(), bits);
                __ Bsl(invalid.V16B(), indefinite.V16B(), wide.V16B());
                __ Orr(wide.V16B(), invalid.V16B(), invalid.V16B());
            }
            __ Xtn(result.V2S(), wide.V2D());
            break;
        case 6: __ Fcvtl(result.V2D(), source.V2S()); break;
        case 7: __ Fcvtn(result.V2S(), source.V2D()); break;
    }
}

void JitTranslator::EmitAsrValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Asr(result, context.X(value), context.X(amount));
    } else {
        __ Asr(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitBitClear(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto lsb = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto value_reg = context.R(value);
    auto result = context.R(ir::Value{inst});
    if (value_reg != result) {
        __ Mov(result, value_reg);
    }
    __ Bfc(result, lsb, bits);
}

void JitTranslator::EmitLslValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Lsl(result, context.X(value), context.X(amount));
    } else {
        __ Lsl(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitLsrValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Lsr(result, context.X(value), context.X(amount));
    } else {
        __ Lsr(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitRorValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Ror(result, context.X(value), context.X(amount));
    } else {
        __ Ror(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitTestZero(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.R(value), 0);
    __ Cset(result, eq);
}

void JitTranslator::EmitBitInsert(ir::Inst* inst) {
    auto dest = inst->GetArg<ir::Value>(0);
    auto src = inst->GetArg<ir::Value>(1);
    auto lsb = inst->GetArg<ir::Imm>(2).Get();
    auto bits = inst->GetArg<ir::Imm>(3).Get();
    auto result = context.R(ir::Value{inst});
    auto dest_reg = context.R(dest);
    if (result != dest_reg) {
        __ Mov(result, dest_reg);
    }
    __ Bfi(result, context.R(src), lsb, bits);
}

bool JitTranslator::ReproveWidthChainBridge(ir::Inst* inst) const {
    if (!inst || !context.IsWidthChainCoalesced(inst->Id())) {
        return false;
    }
    ir::Value source{};
    if (inst->GetOp() == ir::OpCode::BitExtract &&
        ir::GetValueSizeByte(inst->ReturnType()) == sizeof(u32) &&
        inst->GetArg<ir::Imm>(1).Get() == 0 &&
        inst->GetArg<ir::Imm>(2).Get() == 32) {
        source = ResolveWidthChainBitCast(inst->GetArg<ir::Value>(0));
    } else if (inst->GetOp() == ir::OpCode::ZeroExtend32To64 &&
               ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) == sizeof(u32)) {
        source = ResolveWidthChainBitCast(inst->GetArg<ir::Value>(0));
    } else {
        return false;
    }
    // LONG 的 invariant 输入是 pinned X 的 W view。它只在唯一 U32
    // Add/Xor consumer 上省 identity 快照，不宣称 X[63:32] 已清零。
    bool long_u32_snapshot = false;
    if (context.GetFeatures().ra_width_chain_long && source.Defined() &&
        source.Def() && source.Def()->GetOp() == ir::OpCode::GetHostGPR &&
        ir::GetValueSizeByte(source.Type()) == sizeof(u32) && inst->GetUses() == 1) {
        u32 consumers = 0;
        for (auto& scan : cur_block->GetInstList()) {
            for (auto input : scan.GetValues()) {
                if (ResolveWidthChainBitCast(input).Def() != inst) {
                    continue;
                }
                ++consumers;
                long_u32_snapshot |=
                        (scan.GetOp() == ir::OpCode::Add ||
                         scan.GetOp() == ir::OpCode::Xor) &&
                        ir::GetValueSizeByte(scan.ReturnType()) == sizeof(u32);
            }
        }
        long_u32_snapshot &= consumers == 1;
    }
    if (!source.Defined() ||
        (!IsKnownWidthChainWWrite(source, context) && !long_u32_snapshot) ||
        !context.SharesGPR(source, ir::Value{inst})) {
        return false;
    }
    const u32 anchor = context.WidthChainAnchor(inst->Id());
    const u32 source_anchor = context.IsWidthChainCoalesced(source.Id())
            ? context.WidthChainAnchor(source.Id())
            : source.Id();
    if (anchor != source_anchor) {
        return false;
    }

    auto same_component = [&](ir::Value value) {
        value = ResolveWidthChainBitCast(value);
        if (!value.Defined()) {
            return false;
        }
        return value.Id() == anchor ||
               (context.IsWidthChainCoalesced(value.Id()) &&
                context.WidthChainAnchor(value.Id()) == anchor);
    };
    auto last_use = [&](ir::Inst* definition) {
        u32 end = definition->Id();
        for (auto& scan : cur_block->GetInstList()) {
            for (auto value : scan.GetValues()) {
                if (ResolveWidthChainBitCast(value).Def() == definition) {
                    end = std::max<u32>(end, scan.Id());
                }
            }
        }
        if (TerminalUsesWidthChainValue(cur_block->GetTerminal(), definition) &&
            cur_block->GetInstList().begin() != cur_block->GetInstList().end()) {
            end = std::max<u32>(end, std::prev(cur_block->GetInstList().end())->Id());
        }
        return end;
    };

    const u32 end = last_use(inst);
    const u32 target = context.X(source).GetCode();
    const u32 last_id = cur_block->GetInstList().begin() ==
                                cur_block->GetInstList().end()
            ? 0
            : std::prev(cur_block->GetInstList().end())->Id();
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() < inst->Id() || scan.Id() > end) {
            continue;
        }
        if (!context.DirtyGPR(scan.Id()).Get(target) ||
            (backend::FixedGPRClobbers(scan, context.GetFeatures()) &
             (1u << target)) ||
            (scan.Id() == last_id && target == 11 &&
             backend::ScratchXPoolEnabled(context.GetFeatures()))) {
            return false;
        }
        if (scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Imm>(1).Get() == target &&
            !same_component(scan.GetArg<ir::Value>(0))) {
            return false;
        }
    }
    for (auto& other : cur_block->GetInstList()) {
        if (&other == inst || !other.HasValue() || other.IsBitCastOperation() ||
            same_component(ir::Value{&other}) ||
            !context.SharesGPR(ir::Value{&other}, source)) {
            continue;
        }
        if (other.Id() <= end && last_use(&other) >= inst->Id()) {
            const bool exact_last_use_handoff =
                    other.Id() == end && std::any_of(
                            other.GetValues().begin(), other.GetValues().end(),
                            [&](ir::Value input) {
                                return ResolveWidthChainBitCast(input).Def() == inst;
                            });
            if (exact_last_use_handoff) {
                continue;
            }
            return false;
        }
    }
    return true;
}

void JitTranslator::EmitBitExtract(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto result = context.R(ir::Value{inst});
    if (context.IsWidthChainCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveWidthChainBridge(inst),
                   "width-chain BitExtract proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (shift_imm_fast && left == 0 &&
        bits == ir::GetValueSizeByte(inst->ReturnType()) * 8 &&
        context.SharesGPR(value, ir::Value{inst})) {
        return;
    }
    __ Ubfx(result, context.R(value), left, bits);
}

void JitTranslator::EmitSignExtend(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.R(ir::Value{inst});
    auto src = context.W(value);
    switch (ir::GetValueSizeByte(value.Type())) {
        case 1:
            __ Sxtb(result, src);
            break;
        case 2:
            __ Sxth(result, src);
            break;
        case 4:
            if (result.Is64Bits()) {
                __ Sxtw(result, src);
            } else {
                __ Mov(result, src);
            }
            break;
        case 8:
            __ Mov(result, context.X(value));
            break;
        default:
            PANIC();
    }
}

void JitTranslator::EmitTestNotZero(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.R(value), 0);
    __ Cset(result, ne);
}

void JitTranslator::EmitZeroExtend32(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    auto fused = value.Def() ? fused_pin_gpr_reads.find(value.Def())
                             : fused_pin_gpr_reads.end();
    auto src = fused != fused_pin_gpr_reads.end()
            ? WRegister(fused->second)
            : context.W(value);
    if (shift_imm_fast && value.Def() &&
        value.Def()->GetOp() == ir::OpCode::LoadUniform &&
        ir::GetValueSizeByte(value.Type()) <= 2 &&
        context.SharesGPR(value, ir::Value{inst})) {
        // LDRB/LDRH already wrote a W register and therefore already provided
        // the zero extension needed by an immediate GPR shift.
        return;
    }
    if (value.Def() && value.Def()->GetOp() == ir::OpCode::BitExtract &&
        context.SharesGPR(value, ir::Value{inst})) {
        auto extracted = value.Def();
        auto source = extracted->GetArg<ir::Value>(0);
        if (extracted->GetArg<ir::Imm>(1).Get() == 0 &&
            extracted->GetArg<ir::Imm>(2).Get() ==
                    ir::GetValueSizeByte(value.Type()) * 8 &&
            source.Def() && source.Def()->GetOp() == ir::OpCode::GetHostGPR &&
            source.Def()->GetArg<ir::Imm>(0).Get() <= 9) {
            // UBFX already zero-filled the tied W destination.
            return;
        }
    }
    switch (ir::GetValueSizeByte(value.Type())) {
        case 1:
            __ Uxtb(result, src);
            break;
        case 2:
            __ Uxth(result, src);
            break;
        default:
            if (result != src) {
                __ Mov(result, src);
            }
            break;
    }
}

void JitTranslator::EmitZeroExtend32To64(ir::Inst* inst) {
    auto source = inst->GetArg<ir::Value>(0);
    if (context.IsWidthChainCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveWidthChainBridge(inst),
                   "width-chain ZeroExtend32To64 proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (ir::GetValueSizeByte(source.Type()) == sizeof(u32) &&
        context.SharesGPR(source, ir::Value{inst})) {
        return;
    }
    if (inst->GetUses() == 1) {
        auto& list = cur_block->GetInstList();
        for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
            bool names_value = false;
            for (auto used : it->GetValues()) {
                names_value |= used.Def() == inst;
            }
            if (!names_value) {
                continue;
            }
            if (it->GetOp() == ir::OpCode::SetHostGPR &&
                it->GetArg<ir::Value>(0).Def() == inst &&
                it->GetArg<ir::Imm>(2).Get() == 0) {
                const u32 target = it->GetArg<ir::Imm>(1).Get();
                if (target <= 9 || target == 22 || target == 23 || target == 29) {
                    fused_pin_zext32.insert(inst);
                    return;
                }
            }
            break;
        }
    }
    // The destination remains U64-typed in IR so the following StoreUniform
    // updates the full guest GPR. On arm64, writing W is exactly the required
    // 32->64 zero extension and clears the paired X register's high half.
    EmitZeroExtend32(inst);
}

void JitTranslator::EmitZeroExtend64(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.X(ir::Value{inst});
    switch (ir::GetValueSizeByte(value.Type())) {
        case 1:
            __ Uxtb(result.W(), context.W(value));
            break;
        case 2:
            __ Uxth(result.W(), context.W(value));
            break;
        case 4:
            __ Mov(result.W(), context.W(value));
            break;
        default:
            if (result != context.X(value)) {
                __ Mov(result, context.X(value));
            }
            break;
    }
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
    auto multiplier = MaterializeOperand(EmitOperand(right), type);

    auto pseudo_flags = GetPseudoFlags(inst);

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

// De-interleave {left, right}, keeping the even (UZP1) or odd (UZP2) lanes.
// At 64-bit lanes UZP1/UZP2 coincide with ZIP1/ZIP2; the opcode still accepts
// that width so a caller does not have to special-case it.
void JitTranslator::EmitVecUnzip(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool odd = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (odd)
                __ Uzp2(result.V16B(), left.V16B(), right.V16B());
            else
                __ Uzp1(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (odd)
                __ Uzp2(result.V8H(), left.V8H(), right.V8H());
            else
                __ Uzp1(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (odd)
                __ Uzp2(result.V4S(), left.V4S(), right.V4S());
            else
                __ Uzp1(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            if (odd)
                __ Uzp2(result.V2D(), left.V2D(), right.V2D());
            else
                __ Uzp1(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

// Fused multiply-add: dst = +-(a*b) +- c with a SINGLE rounding (ir.inc).
//
// AArch64 has exactly the two accumulate forms, both fused:
//     FMLA Vd, Vn, Vm   ->  Vd = Vd + Vn*Vm
//     FMLS Vd, Vn, Vm   ->  Vd = Vd - Vn*Vm
// so the sign of the PRODUCT picks the mnemonic and the sign of the ADDEND is
// applied to the accumulator up front with FNEG.  Negating the addend before
// the fused step is exact for every finite value and for infinities, so it
// cannot introduce a rounding difference; it does flip the sign bit of a NaN
// addend, which is why the NaN fixup below reads the ORIGINAL c and not the
// negated accumulator.
//
// WHY THE NaN FIXUP IS VECTOR CODE AND NOT THE USUAL LANE LOOP
// ------------------------------------------------------------
// EmitVecFloatNaNFixup extracts every lane into GPRs; at three operands that
// would need half again as many live GPRs as the two-operand version, which
// already had to be cut from 16 temporaries to 8 to stop exhausting the pool.
// FCMEQ(x, x) is false exactly for NaN, so the whole predicate is available in
// the vector unit for one instruction per operand and this emitter needs no
// GPR at all beyond the two immediate materializations.
//
// The x86 rule being reproduced (same as VecFloatBinary / EmitVecFloatNaNFixup):
// a NaN source is returned quieted, earlier sources win over later ones, and an
// invalid operation with no NaN source (Inf*0, or Inf added to the opposite
// Inf) returns the QNaN indefinite -- whose sign bit is SET, unlike ARM's
// default NaN.  Source order here is the arithmetic order a, b, c.
void JitTranslator::EmitVecFMulAdd(ir::Inst* inst) {
    auto a = context.V(inst->GetArg<ir::Value>(0));
    auto b = context.V(inst->GetArg<ir::Value>(1));
    auto c = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(3).Get();
    const u32 flags = inst->GetArg<ir::Imm>(4).Get();
    const bool negate_product = (flags & 1u) != 0;
    const bool negate_addend = (flags & 2u) != 0;
    ASSERT(lane_bits == 32 || lane_bits == 64);

    auto acc = context.GetTmpV();
    auto ok = context.GetTmpV();      // lanes where every source is non-NaN
    auto mask = context.GetTmpV();    // scratch predicate
    auto value = context.GetTmpV();   // scratch replacement value
    auto imm = context.GetTmpX();

    const auto fmt_of = [&](const VRegister& v) { return lane_bits == 32 ? v.V4S() : v.V2D(); };

    // acc = (negate_addend ? -c : c), then the fused step.
    if (negate_addend) {
        __ Fneg(fmt_of(acc), fmt_of(c));
    } else {
        __ Orr(acc.V16B(), c.V16B(), c.V16B());
    }
    if (negate_product) {
        __ Fmls(fmt_of(acc), fmt_of(a), fmt_of(b));
    } else {
        __ Fmla(fmt_of(acc), fmt_of(a), fmt_of(b));
    }

    // ok = ~isnan(a) & ~isnan(b) & ~isnan(c)
    __ Fcmeq(fmt_of(ok), fmt_of(a), fmt_of(a));
    __ Fcmeq(fmt_of(mask), fmt_of(b), fmt_of(b));
    __ And(ok.V16B(), ok.V16B(), mask.V16B());
    __ Fcmeq(fmt_of(mask), fmt_of(c), fmt_of(c));
    __ And(ok.V16B(), ok.V16B(), mask.V16B());

    // A NaN produced by the operation itself (no NaN source) is x86's QNaN
    // indefinite.  mask = ok & isnan(acc).
    __ Fcmeq(fmt_of(mask), fmt_of(acc), fmt_of(acc));
    __ Bic(mask.V16B(), ok.V16B(), mask.V16B());
    if (lane_bits == 32) {
        __ Mov(imm.W(), 0xFFC00000u);
        __ Dup(value.V4S(), imm.W());
    } else {
        __ Mov(imm, UINT64_C(0xFFF8000000000000));
        __ Dup(value.V2D(), imm);
    }
    // BIT: result lanes selected by `mask` take `value`.
    __ Bit(acc.V16B(), value.V16B(), mask.V16B());

    // Quieted source propagation, applied lowest priority first so that a wins.
    if (lane_bits == 32) {
        __ Mov(imm.W(), 0x00400000u);
        __ Dup(value.V4S(), imm.W());
    } else {
        __ Mov(imm, UINT64_C(0x0008000000000000));
        __ Dup(value.V2D(), imm);
    }
    auto quiet = context.GetTmpV();
    __ Orr(quiet.V16B(), value.V16B(), value.V16B());
    for (const VRegister* source : {&c, &b, &a}) {
        __ Fcmeq(fmt_of(mask), fmt_of(*source), fmt_of(*source));
        __ Orr(value.V16B(), source->V16B(), quiet.V16B());
        // BIF: lanes where the predicate is FALSE (i.e. this source is NaN)
        // take `value`.
        __ Bif(acc.V16B(), value.V16B(), mask.V16B());
    }
    __ Orr(result.V16B(), acc.V16B(), acc.V16B());
}

// VecFRoundInt -- round each lane to an integral floating-point value.
//
// The four IR modes map one-for-one onto the AArch64 FRINT family, so this is
// a single instruction per half.  Deliberately NOT FRINTX/FRINTI: those follow
// FPCR, and the IR's mode is an immediate precisely so that the result cannot
// depend on host state the front end never set.
//
// NO NaN FIXUP IS NEEDED HERE, unlike VecFAdd/VecFMul.  EmitVecFloatNaNFixup
// exists because ARM MANUFACTURES a default NaN (7FC0_0000, sign clear) where
// x86 manufactures its indefinite (FFC0_0000, sign set) -- inf-inf, 0*inf and
// friends.  FRINT* cannot manufacture anything: with FPCR.DN clear (this
// runtime never sets it) it returns the operand with the quiet bit forced on
// and every other bit, including the sign, untouched.  That is exactly what
// x86 ROUNDPS/ROUNDPD/ROUNDSS/ROUNDSD do with a NaN operand.  Verified against
// Rosetta on QNaN, SNaN and both signed zeroes -- see avx_misc_test.cpp.
void JitTranslator::EmitVecFRoundInt(ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto merge = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 mode = inst->GetArg<ir::Imm>(3).Get();
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    ASSERT(bits == 32 || bits == 64);
    auto value = context.GetTmpV();
    auto out = VecLaneFormat(value, bits);
    auto in = VecLaneFormat(source, bits);
    switch (mode) {
        case 0:
            __ Frintn(out, in);  // nearest, ties to even
            break;
        case 1:
            __ Frintm(out, in);  // toward -infinity
            break;
        case 2:
            __ Frintp(out, in);  // toward +infinity
            break;
        case 3:
            __ Frintz(out, in);  // toward zero
            break;
        default:
            PANIC("invalid rounding mode");
    }
    // Same merge shape as EmitVecFUnary: lane 0 from the rounded value, the
    // rest of the destination from `merge`.
    if (!scalar) {
        __ Orr(result.V16B(), value.V16B(), value.V16B());
    } else {
        __ Orr(result.V16B(), merge.V16B(), merge.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, value.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, value.V2D(), 0);
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

}  // namespace swift::runtime::backend::arm64
