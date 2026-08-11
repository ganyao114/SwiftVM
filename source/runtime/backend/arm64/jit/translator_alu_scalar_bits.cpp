#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {


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
    if (context.HasWidthComponentOwner(anchor) &&
        (!context.WidthComponentOwnerCommitted(anchor) ||
         context.WidthComponentOwnerTarget(anchor) != target ||
         !context.WidthComponentOwnerHighZero(anchor))) {
        return false;
    }
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


#undef __

}  // namespace swift::runtime::backend::arm64
