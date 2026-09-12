#include "translator.h"

#include <iterator>

namespace swift::runtime::backend::arm64 {

namespace {

bool IsNarrowLoadZeroExtended(ir::Value value, u32 width) {
    for (u32 depth = 0; depth != 8 && value.Defined(); ++depth) {
        auto* definition = value.Def();
        while (definition && definition->IsBitCastOperation()) {
            value = definition->GetArg<ir::Value>(0);
            definition = value.Def();
        }
        if (!definition) {
            return false;
        }
        if (definition->GetOp() == ir::OpCode::LoadMemory ||
            definition->GetOp() == ir::OpCode::LoadUniform) {
            return ir::GetValueSizeByte(definition->ReturnType()) <= width;
        }
        if (definition->GetOp() != ir::OpCode::ZeroExtend32 &&
            definition->GetOp() != ir::OpCode::ZeroExtend32To64) {
            return false;
        }
        value = definition->GetArg<ir::Value>(0);
    }
    return false;
}

}  // namespace

std::optional<JitTranslator::NarrowExtractExtension>
JitTranslator::MatchNarrowExtractExtension(ir::Inst* wrapper) const {
    if (!wrapper || wrapper->GetOp() != ir::OpCode::ZeroExtend32) {
        return std::nullopt;
    }
    auto value = wrapper->GetArg<ir::Value>(0);
    auto* extract = value.Def();
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetUses() != 1 || extract->GetUses(false) != 1 ||
        extract->GetArg<ir::Value>(0).Def() == nullptr ||
        pinned_gprs.fused_pin_gpr_reads.contains(extract) ||
        flag_state.narrow_flags_inputs.contains(extract) ||
        context.IsWidthChainCoalesced(extract->Id()) ||
        context.IsWidthChainCoalesced(wrapper->Id()) ||
        context.IsLow32CopyCoalesced(extract->Id()) ||
        scalar_copy_analysis.InputDiscarded(extract)) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if ((width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        ir::GetValueSizeByte(value.Type()) != width ||
        context.IsSpilled(extract->GetArg<ir::Value>(0))) {
        // A spill reload region may end at the extract and never write its
        // slot. A later wrapper cannot extend that region in the emitter.
        // The IR width pass handles this fusion before allocation instead.
        return std::nullopt;
    }
    auto& list = cur_block->GetInstList();
    auto next = std::next(list.iterator_to(*extract));
    if (next == list.end() || next.operator->() != wrapper) {
        return std::nullopt;
    }
    ir::Inst* shift{};
    auto after_wrapper = std::next(list.iterator_to(*wrapper));
    if (wrapper->GetUses() == 1 && wrapper->GetUses(false) == 1 &&
        after_wrapper != list.end() &&
        after_wrapper->GetOp() == ir::OpCode::LsrImm &&
        after_wrapper->GetArg<ir::Value>(0).Def() == wrapper) {
        const u64 amount = after_wrapper->GetArg<ir::Imm>(1).Get();
        if (amount > 0 && amount < width * 8) {
            shift = after_wrapper.operator->();
        }
    }
    return NarrowExtractExtension{
            .extract = extract,
            .source = extract->GetArg<ir::Value>(0),
            .shift = shift,
            .width = static_cast<u8>(width),
            .source_high_zero = IsNarrowLoadZeroExtended(
                    extract->GetArg<ir::Value>(0), width),
    };
}

std::optional<JitTranslator::NarrowMaskedInput>
JitTranslator::MatchNarrowMaskedInput(ir::Inst* consumer) const {
    if (!consumer || consumer->GetOp() != ir::OpCode::And) {
        return std::nullopt;
    }
    auto left = consumer->GetArg<ir::Value>(0);
    auto* extract = left.Def();
    const u32 width = ir::GetValueSizeByte(consumer->ReturnType());
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        (width != sizeof(u8) && width != sizeof(u16)) ||
        ir::GetValueSizeByte(left.Type()) != width ||
        extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        extract->GetUses() != 1 || extract->GetUses(false) != 1 ||
        !extract->GetArg<ir::Value>(0).Defined() ||
        pinned_gprs.fused_pin_gpr_reads.contains(extract) ||
        flag_state.narrow_flags_inputs.contains(extract) ||
        context.IsWidthChainCoalesced(extract->Id()) ||
        context.IsLow32CopyCoalesced(extract->Id()) ||
        scalar_copy_analysis.InputDiscarded(extract) ||
        context.IsSpilled(extract->GetArg<ir::Value>(0))) {
        return std::nullopt;
    }
    const auto right = consumer->GetArg<ir::Operand>(1);
    u64 mask{};
    if (right.IsImm()) {
        mask = right.GetLeft().imm.Get();
    } else if (right.GetLeft().IsValue() && right.GetRight().Null()) {
        auto* definition = right.GetLeft().value.Def();
        if (!definition || definition->GetOp() != ir::OpCode::LoadImm) {
            return std::nullopt;
        }
        mask = definition->GetArg<ir::Imm>(0).Get();
    } else {
        return std::nullopt;
    }
    if ((mask >> (width * 8)) != 0) {
        return std::nullopt;
    }
    return NarrowMaskedInput{
            .extract = extract,
            .source = extract->GetArg<ir::Value>(0),
            .width = static_cast<u8>(width),
    };
}

std::optional<JitTranslator::ShiftMaskedInput>
JitTranslator::MatchShiftMaskedInput(ir::Inst* consumer) {
    if (!consumer || consumer->GetOp() != ir::OpCode::And ||
        !GetPseudoFlags(consumer).Null()) {
        return std::nullopt;
    }
    const auto right = consumer->GetArg<ir::Operand>(1);
    if (!right.IsImm() || right.GetLeft().imm.Get() != 1) {
        return std::nullopt;
    }
    const auto value = consumer->GetArg<ir::Value>(0);
    auto* shift = value.Def();
    if (!shift ||
        (shift->GetOp() != ir::OpCode::LsrImm &&
         shift->GetOp() != ir::OpCode::AsrImm) ||
        !GetPseudoFlags(shift).Null() || shift->GetUses() != 1 ||
        shift->GetUses(false) != 1 ||
        ir::GetValueSizeByte(value.Type()) !=
                ir::GetValueSizeByte(consumer->ReturnType()) ||
        fused_narrow_extract_shifts.contains(shift) ||
        scalar_copy_analysis.InputDiscarded(shift)) {
        return std::nullopt;
    }
    const auto source = shift->GetArg<ir::Value>(0);
    const u32 width = ir::GetValueSizeByte(source.Type()) * 8;
    const u64 offset = shift->GetArg<ir::Imm>(1).Get();
    if (!source.Defined() || width != ir::GetValueSizeByte(value.Type()) * 8 ||
        offset >= width || !context.HasAllocation(source) ||
        context.IsSpilled(source)) {
        return std::nullopt;
    }
    auto& list = cur_block->GetInstList();
    auto current = list.iterator_to(*consumer);
    if (current == list.begin() || std::prev(current).operator->() != shift) {
        return std::nullopt;
    }
    return ShiftMaskedInput{
            .shift = shift,
            .source = source,
            .offset = static_cast<u8>(offset),
    };
}

void JitTranslator::PrepareNarrowExtractExtensions(ir::Block* block) {
    narrow_extract_extensions.clear();
    fused_narrow_extracts.clear();
    fused_narrow_extract_shifts.clear();
    narrow_masked_inputs.clear();
    fused_narrow_masked_extracts.clear();
    shift_masked_inputs.clear();
    fused_shift_masked_shifts.clear();
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchNarrowExtractExtension(&inst);
        if (!candidate) {
            continue;
        }
        fused_narrow_extracts.emplace(candidate->extract, &inst);
        narrow_extract_extensions.emplace(&inst, *candidate);
        if (candidate->shift) {
            fused_narrow_extract_shifts.emplace(candidate->shift, &inst);
        }
    }
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchNarrowMaskedInput(&inst);
        if (!candidate) {
            continue;
        }
        narrow_masked_inputs.emplace(&inst, *candidate);
        fused_narrow_masked_extracts.emplace(candidate->extract, &inst);
    }
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchShiftMaskedInput(&inst);
        if (!candidate) {
            continue;
        }
        shift_masked_inputs.emplace(&inst, *candidate);
        fused_shift_masked_shifts.emplace(candidate->shift, &inst);
    }
}

}  // namespace swift::runtime::backend::arm64
