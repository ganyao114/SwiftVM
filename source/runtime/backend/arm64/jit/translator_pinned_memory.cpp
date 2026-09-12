#include "translator.h"

#include <limits>

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

bool IsPinnedMemoryTarget(u32 target) {
    return target <= 9 || target == 19 || target == 20 || target == 21 ||
           target == 22 || target == 23 || target == 29;
}

bool IsCallerSavedAddressBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    return op == O::CallLambda || op == O::CallLocation ||
           op == O::CallDynamic || op == O::X87Op || op == O::Sse42Str;
}

}  // namespace

std::optional<JitTranslator::PinnedMemorySource>
JitTranslator::MatchPinnedMemorySource(ir::Value source) const {
    auto* read = source.Def();
    if (!read || ir::GetValueSizeByte(source.Type()) != sizeof(u64)) {
        return std::nullopt;
    }

    u32 target{};
    u32 live_begin{};
    if (read->GetOp() == ir::OpCode::GetHostGPR) {
        if (read->GetArg<ir::Imm>(1).Get() != 0) {
            return std::nullopt;
        }
        target = read->GetArg<ir::Imm>(0).Get();
        live_begin = read->Id();
    } else if (read->GetOp() == ir::OpCode::BitCast) {
        const auto published = read->GetArg<ir::Value>(0);
        auto* producer = published.Def();
        if (!producer || producer->GetOp() != ir::OpCode::LoadMemory ||
            ir::GetValueSizeByte(published.Type()) != sizeof(u64) ||
            read->GetUses(false) != 1) {
            return std::nullopt;
        }
        ir::Inst* publication = nullptr;
        u32 producer_uses = 0;
        for (auto& scan : cur_block->GetInstList()) {
            for (auto used : scan.GetValues()) {
                if (used.Def() != producer) {
                    continue;
                }
                ++producer_uses;
                if (&scan == read) {
                    continue;
                }
                if (publication || scan.GetOp() != ir::OpCode::SetHostGPR ||
                    scan.GetArg<ir::Value>(0).Def() != producer ||
                    scan.GetArg<ir::Imm>(2).Get() != 0 ||
                    !context.IsHostWriteCoalesced(scan.Id())) {
                    return std::nullopt;
                }
                publication = &scan;
            }
        }
        if (!publication || producer_uses != producer->GetUses() ||
            publication->Id() >= read->Id()) {
            return std::nullopt;
        }
        target = publication->GetArg<ir::Imm>(1).Get();
        live_begin = publication->Id();
        if (context.X(published).GetCode() != target) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!IsPinnedMemoryTarget(target) || context.X(source).GetCode() != target) {
        return std::nullopt;
    }
    return PinnedMemorySource{static_cast<u16>(target), live_begin};
}

std::optional<JitTranslator::PinnedMemoryAddress>
JitTranslator::MatchPinnedMemoryAddress(ir::Inst* address) const {
    if (!address || address->GetOp() != ir::OpCode::GetOperand ||
        address->GetUses(false) != 1) {
        return std::nullopt;
    }
    const auto operand = address->GetArg<ir::Operand>(0);
    if (!operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    s64 offset{};
    if (!operand.GetRight().Null()) {
        if (operand.GetOp() != ir::OperandOp::Plus ||
            !operand.GetRight().IsImm()) {
            return std::nullopt;
        }
        offset = operand.GetRight().imm.GetSigned();
        if (offset == std::numeric_limits<s64>::min()) {
            return std::nullopt;
        }
    }
    const auto source = MatchPinnedMemorySource(operand.GetLeft().value);
    if (!source) {
        return std::nullopt;
    }

    ir::Inst* memory = nullptr;
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= address->Id() ||
            (scan.GetOp() != ir::OpCode::LoadMemory &&
             scan.GetOp() != ir::OpCode::StoreMemory)) {
            continue;
        }
        const auto memory_operand = scan.GetArg<ir::Operand>(0);
        if (memory_operand.GetRight().Null() &&
            memory_operand.GetLeft().IsValue() &&
            memory_operand.GetLeft().value.Def() == address) {
            memory = &scan;
            break;
        }
    }
    if (!memory) {
        return std::nullopt;
    }

    if (offset != 0 && !memory_state.use_memory_base) {
        const auto type = memory->GetOp() == ir::OpCode::LoadMemory
                ? memory->ReturnType()
                : memory->GetArg<ir::Value>(1).Type();
        const u32 access_size = ir::GetValueSizeByte(type);
        if (!__ IsImmLSUnscaled(offset) &&
            !__ IsImmLSScaled(offset, access_size)) {
            return std::nullopt;
        }
    }

    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= source->live_begin || scan.Id() >= memory->Id()) {
            continue;
        }
        if ((scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == source->target) ||
            (source->target <= 9 && IsCallerSavedAddressBarrier(scan.GetOp()))) {
            return std::nullopt;
        }
    }
    return PinnedMemoryAddress{memory, source->target, offset};
}

std::optional<u16> JitTranslator::MatchPinnedMemoryValue(ir::Inst* extract) const {
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        extract->GetUses(false) != 1) {
        return std::nullopt;
    }
    const auto source = extract->GetArg<ir::Value>(0);
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if (!source.Def() || (width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        ir::GetValueSizeByte(source.Type()) < width || context.IsSpilled(source)) {
        return std::nullopt;
    }

    ir::Inst* store = nullptr;
    ir::Inst* publication = nullptr;
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Value>(0).Def() == source.Def()) {
            if (publication) {
                return std::nullopt;
            }
            publication = &scan;
        }
        const bool uses_extract = std::ranges::any_of(
                scan.GetValues(), [&](ir::Value value) {
                    return value.Def() == extract;
                });
        if (!uses_extract) {
            continue;
        }
        if (store || scan.GetOp() != ir::OpCode::StoreMemory ||
            scan.GetArg<ir::Value>(1).Def() != extract ||
            ir::GetValueSizeByte(scan.GetArg<ir::Value>(1).Type()) != width) {
            return std::nullopt;
        }
        store = &scan;
    }
    if (!store || !publication || publication->Id() >= extract->Id() ||
        extract->Id() >= store->Id() ||
        publication->GetArg<ir::Imm>(2).Get() != 0 ||
        !context.IsHostWriteCoalesced(publication->Id()) ||
        !ReproveCoalescedHostWrite(publication)) {
        return std::nullopt;
    }

    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    if (!IsPinnedMemoryTarget(target) || context.R(source).GetCode() != target) {
        return std::nullopt;
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= publication->Id() || scan.Id() >= store->Id()) {
            continue;
        }
        if ((scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == target) ||
            (target <= 9 && IsCallerSavedAddressBarrier(scan.GetOp()))) {
            return std::nullopt;
        }
    }
    return static_cast<u16>(target);
}

void JitTranslator::PreparePinnedMemoryValues(ir::Block* block) {
    memory_state.pinned_memory_values.clear();
    for (auto& inst : block->GetInstList()) {
        if (auto target = MatchPinnedMemoryValue(&inst)) {
            memory_state.pinned_memory_values.emplace(&inst, *target);
            pinned_gprs.fused_pin_gpr_reads.emplace(&inst, *target);
        }
    }
}

#undef __

}  // namespace swift::runtime::backend::arm64
