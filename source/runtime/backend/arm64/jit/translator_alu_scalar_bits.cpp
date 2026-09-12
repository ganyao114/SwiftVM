#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

bool HasLaterLocalControlFlow(ir::Block* block, ir::Inst* inst) {
    bool seen = false;
    for (auto& scan : block->GetInstList()) {
        if (!seen) {
            seen = &scan == inst;
            continue;
        }
        if (scan.GetOp() == ir::OpCode::Goto ||
            scan.GetOp() == ir::OpCode::NotGoto ||
            scan.GetOp() == ir::OpCode::BindLabel) {
            return true;
        }
    }
    return false;
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

bool IsPinnedWViewProducer(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
        case ir::OpCode::And:
        case ir::OpCode::AndNot:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
        case ir::OpCode::Mul:
            return true;
        default:
            return false;
    }
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
    if (flag_state.save_in_nzcv && flag_state.nzcv_dirty &&
        !HasLaterLocalControlFlow(cur_block, inst)) {
        EmitZeroTestPreservingPstate(inst, false);
        return;
    }
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
    // Add/Xor consumer 上省 direct 快照，不宣称 X[63:32] 已清零。
    bool long_u32_capture = false;
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
                long_u32_capture |=
                        (scan.GetOp() == ir::OpCode::Add ||
                         scan.GetOp() == ir::OpCode::Xor) &&
                        ir::GetValueSizeByte(scan.ReturnType()) == sizeof(u32);
            }
        }
        long_u32_capture &= consumers == 1;
    }
    if (!source.Defined() ||
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

    const u32 target = context.X(source).GetCode();
    ir::Inst* pinned_consumer = nullptr;
    u32 pinned_uses = 0;
    for (auto& scan : cur_block->GetInstList()) {
        for (auto input : scan.GetValues()) {
            if (ResolveWidthChainBitCast(input).Def() != inst) {
                continue;
            }
            ++pinned_uses;
            pinned_consumer = &scan;
        }
    }
    bool pinned_w_handoff = pinned_uses == 1 && pinned_consumer &&
                            IsPinnedWViewProducer(pinned_consumer->GetOp()) &&
                            ir::GetValueSizeByte(pinned_consumer->ReturnType()) == sizeof(u32) &&
                            last_use(source.Def()) == inst->Id() &&
                            last_use(inst) == pinned_consumer->Id() &&
                            context.SharesGPR(ir::Value{pinned_consumer}, source);
    if (pinned_w_handoff) {
        bool published = false;
        for (auto& scan : cur_block->GetInstList()) {
            if (scan.Id() <= pinned_consumer->Id() || scan.GetOp() != ir::OpCode::SetHostGPR ||
                scan.GetArg<ir::Imm>(1).Get() != target || scan.GetArg<ir::Imm>(2).Get() != 0 ||
                !context.IsHostWriteCoalesced(scan.Id())) {
                continue;
            }
            auto value = ResolveWidthChainBitCast(scan.GetArg<ir::Value>(0));
            if (value.Def() && value.Def()->GetOp() == ir::OpCode::ZeroExtend32To64) {
                value = ResolveWidthChainBitCast(value.Def()->GetArg<ir::Value>(0));
            }
            published |= value.Def() == pinned_consumer;
        }
        pinned_w_handoff = published;
    }
    if (!IsKnownWidthChainWWrite(source, context) && !long_u32_capture && !pinned_w_handoff) {
        return false;
    }

    const u32 end = last_use(inst);
    if (context.HasWidthComponentOwner(anchor) &&
        (!context.WidthComponentOwnerCommitted(anchor) ||
         context.WidthComponentOwnerTarget(anchor) != target ||
         (!pinned_w_handoff && !context.WidthComponentOwnerHighZero(anchor)))) {
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
            auto inputs = other.GetValues();
            const bool exact_last_use_handoff =
                    other.Id() == end && std::any_of(
                            inputs.begin(), inputs.end(),
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

bool JitTranslator::ReproveLow32ViewOwnership(ir::Inst* inst,
                                              ir::Value source) const {
    const u32 source_target = context.X(source).GetCode();
    bool has_consumer = false;
    u32 last_use = inst->Id();
    u32 source_last_use = inst->Id();
    for (auto& scan : cur_block->GetInstList()) {
        for (auto input : scan.GetValues()) {
            if (ResolveWidthChainBitCast(input).Def() == source.Def()) {
                source_last_use = std::max<u32>(source_last_use, scan.Id());
            }
        }
        bool uses = false;
        for (auto input : scan.GetValues()) {
            uses |= input.Def() == inst;
        }
        if (!uses) {
            continue;
        }
        has_consumer = true;
        last_use = std::max<u32>(last_use, scan.Id());
        if (context.HasAllocation(ir::Value{&scan}) &&
            context.SharesGPR(ir::Value{inst}, ir::Value{&scan})) {
            return false;
        }
        if (!context.HasAllocation(ir::Value{&scan}) &&
            scan.GetOp() != ir::OpCode::StoreMemory &&
            scan.GetOp() != ir::OpCode::StoreUniform &&
            scan.GetOp() != ir::OpCode::SetHostGPR) {
            return false;
        }
    }
    if (!has_consumer ||
        (source_last_use < last_use && source_last_use != inst->Id())) {
        return false;
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() < inst->Id()) {
            continue;
        }
        if (scan.Id() > last_use) {
            break;
        }
        if (!context.DirtyGPR(scan.Id()).Get(source_target)) {
            return false;
        }
    }
    return true;
}

bool JitTranslator::ReproveAdjacentLow32Copy(ir::Inst* inst,
                                              ir::Value source) const {
    if (inst->GetUses() != 1) {
        return false;
    }
    const u32 source_target = context.X(source).GetCode();
    if (!context.DirtyGPR(inst->Id()).Get(source_target)) {
        return false;
    }

    auto& list = cur_block->GetInstList();
    auto wrapper_it = std::next(list.iterator_to(*inst));
    if (wrapper_it == list.end()) {
        return false;
    }
    auto& wrapper = *wrapper_it;
    if (wrapper.GetOp() != ir::OpCode::ZeroExtend32To64 ||
        wrapper.GetArg<ir::Value>(0).Def() != inst ||
        context.X(ir::Value{&wrapper}).GetCode() == source_target) {
        return false;
    }

    return !context.IsWidthChainCoalesced(wrapper.Id()) &&
           context.DirtyGPR(wrapper.Id()).Get(source_target);
}

bool JitTranslator::ReproveLow32Copy(ir::Inst* inst) const {
    if (!inst || !context.IsLow32CopyCoalesced(inst->Id()) ||
        inst->GetOp() != ir::OpCode::BitExtract ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u32) ||
        inst->GetArg<ir::Imm>(1).Get() != 0 ||
        inst->GetArg<ir::Imm>(2).Get() != 32) {
        return false;
    }
    auto source = ResolveWidthChainBitCast(inst->GetArg<ir::Value>(0));
    if (!source.Defined() || context.Low32CopySource(inst->Id()) != source.Id() ||
        !context.SharesGPR(source, ir::Value{inst})) {
        return false;
    }
    return ReproveLow32ViewOwnership(inst, source) ||
           ReproveAdjacentLow32Copy(inst, source);
}

void JitTranslator::EmitBitExtract(ir::Inst* inst) {
    if (auto fused = fused_narrow_masked_extracts.find(inst);
        fused != fused_narrow_masked_extracts.end()) {
        const auto candidate = narrow_masked_inputs.find(fused->second);
        const auto reproved = MatchNarrowMaskedInput(fused->second);
        ASSERT_MSG(candidate != narrow_masked_inputs.end() && reproved &&
                           *reproved == candidate->second && reproved->extract == inst,
                   "narrow masked input proof diverged at IR {}", inst->Id());
        return;
    }
    if (auto value = memory_state.pinned_memory_values.find(inst);
        value != memory_state.pinned_memory_values.end()) {
        ASSERT_MSG(MatchPinnedMemoryValue(inst) == value->second,
                   "pinned memory value proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (auto fused = fused_narrow_extracts.find(inst);
        fused != fused_narrow_extracts.end()) {
        const auto candidate = narrow_extract_extensions.find(fused->second);
        const auto reproved = MatchNarrowExtractExtension(fused->second);
        ASSERT_MSG(candidate != narrow_extract_extensions.end() && reproved &&
                           *reproved == candidate->second &&
                           reproved->extract == inst,
                   "narrow extract extension proof diverged at IR {}", inst->Id());
        return;
    }
    if (pinned_gprs.fused_pin_gpr_reads.contains(inst)) {
        return;
    }
    if (flag_state.narrow_flags_inputs.contains(inst)) {
        ASSERT_MSG(MatchNarrowFlagsInput(inst) == flag_state.narrow_flags_inputs.at(inst),
                   "narrow flags input proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (scalar_copy_analysis.InputDiscarded(inst)) {
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto result = [&]() -> Register {
        if (auto pinned = pinned_gprs.pinned_gpr_values.find(inst);
            pinned != pinned_gprs.pinned_gpr_values.end()) {
            return XRegister(pinned->second);
        }
        return context.R(ir::Value{inst});
    }();
    if (context.IsWidthChainCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveWidthChainBridge(inst),
                   "width-chain BitExtract proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (context.IsLow32CopyCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveLow32Copy(inst),
                   "low32 copy proof drifted before emission at IR {}",
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
    if (fused_pin_sign_extends.contains(inst)) {
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    const auto pinned = ResolvePinnedGPRValue(ir::Value{inst});
    auto result = pinned ? Register{*pinned} : context.R(ir::Value{inst});
    const auto residence = guest_state_map.FixedHomeForUse(value, inst);
    auto fused = value.Def() ? pinned_gprs.fused_pin_gpr_reads.find(value.Def())
                             : pinned_gprs.fused_pin_gpr_reads.end();
    auto src = residence
            ? WRegister(residence->home)
            : fused != pinned_gprs.fused_pin_gpr_reads.end()
            ? WRegister(fused->second)
            : context.W(value);
    const u32 source_width = ir::GetValueSizeByte(value.Type());
    const u32 source_bits = source_width * 8;
    const u32 result_bits = result.Is64Bits() ? 64 : 32;
    if (residence &&
        residence->extension.KnownSignExtended(source_bits, result_bits)) {
        const auto resident = result.Is64Bits()
                ? Register{XRegister(residence->home)}
                : Register{WRegister(residence->home)};
        if (result != resident) {
            __ Mov(result, resident);
        }
        return;
    }
    switch (source_width) {
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

bool JitTranslator::CanFusePinnedZeroExtendPublication(ir::Inst* inst) {
    if (!inst) {
        return false;
    }
    if (fused_pin_zext32.contains(inst)) {
        return true;
    }
    if (!checked_pin_zext32_publications.insert(inst).second ||
        inst->GetUses() != 1) {
        return false;
    }
    auto& list = cur_block->GetInstList();
    for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
        bool names_value = false;
        for (auto used : it->GetValues()) {
            names_value |= used.Def() == inst;
        }
        if (!names_value) {
            continue;
        }
        if (it->GetOp() != ir::OpCode::SetHostGPR ||
            it->GetArg<ir::Value>(0).Def() != inst ||
            it->GetArg<ir::Imm>(2).Get() != 0) {
            return false;
        }
        const u32 target = it->GetArg<ir::Imm>(1).Get();
        const bool fused = target <= 9 || target == 22 || target == 23 ||
                           target == 29;
        if (fused) {
            fused_pin_zext32.insert(inst);
        }
        return fused;
    }
    return false;
}

bool JitTranslator::CanConsumeForwardedWidthSpill(ir::Inst* inst) {
    if (!inst) {
        return false;
    }
    const auto reads_fixed_home = [&] {
        const auto source = inst->GetArg<ir::Value>(0);
        return guest_state_map.FixedHomeForUse(source, inst).has_value() ||
               (source.Def() && pinned_gprs.fused_pin_gpr_reads.contains(source.Def()));
    };
    switch (inst->GetOp()) {
        case ir::OpCode::ZeroExtend32:
            return !CanUseZeroStoreRegister(ir::Value{inst}) &&
                   !reads_fixed_home() && !fused_pin_zext32.contains(inst) &&
                   !narrow_extract_extensions.contains(inst);
        case ir::OpCode::ZeroExtend32To64: {
            const auto source = inst->GetArg<ir::Value>(0);
            return !CanUseZeroStoreRegister(ir::Value{inst}) &&
                   !reads_fixed_home() && !fused_pin_zext32.contains(inst) &&
                   !context.IsWidthChainCoalesced(inst->Id()) &&
                   !(source.Def() && context.IsLow32CopyCoalesced(source.Id())) &&
                   !CanFusePinnedZeroExtendPublication(inst);
        }
        case ir::OpCode::ZeroExtend64:
            return true;
        case ir::OpCode::SignExtend:
            return !reads_fixed_home() && !fused_pin_sign_extends.contains(inst);
        default:
            return false;
    }
}

void JitTranslator::EmitTestNotZero(ir::Inst* inst) {
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
    if (flag_state.save_in_nzcv && flag_state.nzcv_dirty &&
        !HasLaterLocalControlFlow(cur_block, inst)) {
        EmitZeroTestPreservingPstate(inst, true);
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.R(value), 0);
    __ Cset(result, ne);
}

void JitTranslator::EmitZeroTestPreservingPstate(ir::Inst* inst, bool nonzero) {
    const auto value = context.R(inst->GetArg<ir::Value>(0));
    const auto result = context.X(ir::Value{inst});
    const auto scratch = context.GetSharedTmpX();
    if (value.Is64Bits()) {
        __ Clz(scratch, value);
        __ Lsr(result, scratch, 6);
    } else {
        __ Clz(scratch.W(), value.W());
        __ Lsr(result.W(), scratch.W(), 5);
    }
    if (nonzero) {
        __ Eor(result.W(), result.W(), 1);
    }
}

void JitTranslator::EmitZeroExtend32(ir::Inst* inst) {
    if (CanUseZeroStoreRegister(ir::Value{inst})) {
        return;
    }
    if (fused_pin_zext32.contains(inst)) {
        return;
    }
    if (auto fused = narrow_extract_extensions.find(inst);
        fused != narrow_extract_extensions.end()) {
        const auto reproved = MatchNarrowExtractExtension(inst);
        ASSERT_MSG(reproved && *reproved == fused->second,
                   "narrow extract extension proof diverged at IR {}", inst->Id());
        if (fused->second.shift) {
            return;
        }
        auto result = context.W(ir::Value{inst});
        auto source = context.W(fused->second.source);
        if (fused->second.source_high_zero && result == source) {
            return;
        }
        if (fused->second.width == sizeof(u8)) {
            __ Uxtb(result, source);
        } else {
            __ Uxth(result, source);
        }
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    const auto residence = guest_state_map.FixedHomeForUse(value, inst);
    auto fused = value.Def() ? pinned_gprs.fused_pin_gpr_reads.find(value.Def())
                             : pinned_gprs.fused_pin_gpr_reads.end();
    auto src = residence
            ? WRegister(residence->home)
            : fused != pinned_gprs.fused_pin_gpr_reads.end()
            ? WRegister(fused->second)
            : context.W(value);
    const u32 source_bits = ir::GetValueSizeByte(value.Type()) * 8;
    if (residence && residence->extension.KnownZeroAbove(source_bits)) {
        if (result != src) {
            __ Mov(result, src);
        }
        return;
    }
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
    const bool no_elide = GetSvmConfig().zext_no_elide;
    if (!no_elide && CanUseZeroStoreRegister(ir::Value{inst})) {
        return;
    }
    if (!no_elide && fused_pin_zext32.contains(inst)) {
        return;
    }
    auto source = inst->GetArg<ir::Value>(0);
    if (!no_elide && context.IsWidthChainCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveWidthChainBridge(inst),
                   "width-chain ZeroExtend32To64 proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (!no_elide && source.Def() &&
        context.IsLow32CopyCoalesced(source.Id())) {
        ASSERT_MSG(ReproveLow32Copy(source.Def()),
                   "low32 copy proof drifted at ZeroExtend32To64 IR {}",
                   inst->Id());
        EmitZeroExtend32(inst);
        return;
    }
    if (!no_elide && ir::GetValueSizeByte(source.Type()) == sizeof(u32) &&
        context.SharesGPR(source, ir::Value{inst})) {
        // Tied to the same register: the W write that clears [63:32] is still
        // owed. The source register may be a pinned-home U32 view whose upper
        // half holds the live guest value, so mov wN,wN is not optional.
        auto result = context.W(ir::Value{inst});
        __ Mov(result, result);
        return;
    }
    if (!no_elide && CanFusePinnedZeroExtendPublication(inst)) {
        return;
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
