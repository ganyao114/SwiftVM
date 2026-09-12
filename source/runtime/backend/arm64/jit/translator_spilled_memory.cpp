#include "translator.h"

namespace swift::runtime::backend::arm64 {

std::optional<u32> JitTranslator::ForwardedMemorySpillInput(ir::Inst* inst) {
    const auto pending = context.PendingScalarSpillValue();
    if (!inst || !pending) {
        return std::nullopt;
    }

    const auto operand_reads = [&](ir::Operand operand,
                                   bool allow_writeback) {
        if (operand.GetRight().Null()) {
            if (!operand.GetLeft().IsValue()) {
                return false;
            }
            const auto address = operand.GetLeft().value;
            if (address.Id() != *pending) {
                return false;
            }
            if (address.Def()) {
                const auto recomputed = memory_state.spilled_memory_operands.find(
                        address.Def());
                if (recomputed != memory_state.spilled_memory_operands.end() &&
                    recomputed->second.consumer == inst) {
                    return false;
                }
            }
            if (!memory_state.use_memory_base &&
                context.IsConstAddressCached(address.Id())) {
                return true;
            }
            if (allow_writeback && address.Def()) {
                const auto update = memory_state.use_memory_base
                        ? MatchBiasedMemoryUpdate(address.Def())
                        : MatchPreIndexMemoryUpdate(address.Def());
                if (update && update->memory == inst) {
                    return false;
                }
            }
            return !ResolvePinnedGPRValue(address).has_value();
        }

        const auto side_reads = [&](const ir::DataClass& side) {
            return side.IsValue() && side.value.Id() == *pending &&
                   !ResolvePinnedGPRValue(side.value).has_value();
        };
        return side_reads(operand.GetLeft()) ||
               side_reads(operand.GetRight());
    };

    switch (inst->GetOp()) {
        case ir::OpCode::LoadMemory: {
            const auto result = ir::Value{inst};
            if ((context.IsSpilled(result) &&
                 !ResolvePinnedGPRValue(result) &&
                 context.HasSpillReloadAtDefinition(inst)) ||
                pinned_gprs.pinned_load_updates.contains(inst)) {
                return std::nullopt;
            }
            const auto operand = inst->GetArg<ir::Operand>(0);
            return operand_reads(
                           operand,
                           inst->ReturnType() != ir::ValueType::V128)
                    ? pending
                    : std::nullopt;
        }
        case ir::OpCode::StoreMemory: {
            const auto value = inst->GetArg<ir::Value>(1);
            if (operand_reads(inst->GetArg<ir::Operand>(0),
                              value.Type() != ir::ValueType::V128)) {
                return pending;
            }
            if (value.Id() != *pending ||
                ir::IsFloatValueType(value.Type()) ||
                resident_scalar_fpr_analysis.FindMemoryStore(inst)) {
                return std::nullopt;
            }
            const u32 width = ir::GetValueSizeByte(value.Type());
            if (width <= sizeof(u32)) {
                const auto residence = guest_state_map.FixedHomeForUse(
                        value, inst);
                if (CanUseZeroStoreRegister(value) ||
                    (residence && residence->width == width) ||
                    (value.Def() &&
                     (memory_state.pinned_memory_values.contains(value.Def()) ||
                      pinned_gprs.fused_pin_gpr_reads.contains(value.Def())))) {
                    return std::nullopt;
                }
            }
            return pending;
        }
        case ir::OpCode::StoreMemoryTSO: {
            const auto value = inst->GetArg<ir::Value>(1);
            return value.Id() == *pending &&
                           !ir::IsFloatValueType(value.Type())
                    ? pending
                    : std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

}  // namespace swift::runtime::backend::arm64
