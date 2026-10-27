#include "translator.h"

namespace swift::runtime::backend::arm64 {

std::optional<u32> JitTranslator::ForwardedMemorySpillInput(const ir::Inst* inst) {
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
                    recomputed->second.consumer == inst->Id()) {
                    return false;
                }
            }
            if (allow_writeback && address.Def()) {
                const auto update =
                        memory_state.use_memory_base
                                ? PreparedMemoryUpdate(address.Def(), MemoryTransferAction::Biased)
                                : PreparedMemoryUpdate(address.Def(),
                                                       MemoryTransferAction::PreIndex);
                if (update && update->MatchesMemory(*inst)) {
                    return false;
                }
            }
            return !context.PlannedGPR<XRegister>(address, inst).has_value();
        }

        const auto side_reads = [&](const ir::DataClass& side) {
            return side.IsValue() && side.value.Id() == *pending &&
                   !context.PlannedGPR<XRegister>(side.value, inst).has_value();
        };
        return side_reads(operand.GetLeft()) ||
               side_reads(operand.GetRight());
    };

    switch (inst->GetOp()) {
        case ir::OpCode::LoadMemory: {
            const auto result = ir::Value{inst};
            if ((context.IsSpilled(result) && !context.PlannedGPR<XRegister>(result, inst) &&
                 context.HasSpillReloadAtDefinition(inst)) ||
                (context.GetActions().MemoryTransfer(inst->Id()) &&
                 context.GetActions().MemoryTransfer(inst->Id())->kind ==
                         MemoryTransferAction::Load)) {
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
            if (value.Id() != *pending || ir::IsFloatValueType(value.Type()) ||
                context.GetActions().ScalarFPRStore(inst->Id())) {
                return std::nullopt;
            }
            const u32 width = ir::GetValueSizeByte(value.Type());
            if (width <= sizeof(u32)) {
                const auto residence = FixedGPRHomeForUse(
                        value, inst);
                if (CanUseZeroStoreRegister(value) || (residence && residence->width == width) ||
                    (value.Def() && ((context.GetActions().MemoryTransfer(value.Id()) &&
                                      context.GetActions()
                                              .MemoryTransfer(value.Id())
                                              ->MatchesValue(*inst, value)) ||
                                     HasGPRLowView(value.Def())))) {
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
