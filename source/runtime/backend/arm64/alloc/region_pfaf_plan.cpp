#include "region_pfaf_plan.h"

namespace swift::runtime::backend::arm64 {
std::optional<RegionPFAFPlan> BuildRegionPFAFPlan(const ir::Block& block,
        bool dead_successor, const ir::Inst* producer, const ir::Inst* final_save) {
    if (!dead_successor || !producer || !final_save || final_save->GetOp() != ir::OpCode::SaveFlags ||
        final_save->GetArg<ir::Value>(0).Def() != producer || producer->GetOp() != ir::OpCode::Sub ||
        (producer->ReturnType() != ir::ValueType::U8 && producer->ReturnType() != ir::ValueType::U16))
        return std::nullopt;
    const auto requested = final_save->GetArg<ir::Flags>(1);
    if (!True(requested & ir::Flags::NZCV) || !True(requested & ir::Flags::Parity) ||
        !True(requested & ir::Flags::AuxiliaryCarry)) return std::nullopt;
    RegionPFAFPlan plan{.producer = producer, .width = static_cast<u8>(ir::GetValueSizeByte(producer->ReturnType()))};
    using Operand = RegionPFAFPlan::Operand;
    const auto describe = [&](ir::Value value) -> Operand {
        auto* def = value.Def();
        if (!def || ir::GetValueSizeByte(value.Type()) < plan.width) return {};
        if (def->GetOp() == ir::OpCode::LoadImm) return {Operand::Kind::Imm, def->GetArg<ir::Imm>(0).Get(), 0};
        if (def->GetOp() == ir::OpCode::GetHostGPR) {
            const auto offset = def->GetArg<ir::Imm>(1).Get();
            if (offset + plan.width > 8) return {};
            return {Operand::Kind::HostGPR, def->GetArg<ir::Imm>(0).Get(), static_cast<u8>(offset)};
        }
        if (def->GetOp() == ir::OpCode::LoadUniform) {
            const auto uniform = def->GetArg<ir::Uniform>(0);
            if (ir::GetValueSizeByte(uniform.GetType()) < plan.width) return {};
            return {Operand::Kind::Uniform, uniform.GetOffset(), 0};
        }
        return {};
    };
    plan.left = describe(producer->GetArg<ir::Value>(0));
    const auto right = producer->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null()) return std::nullopt;
    if (right.GetLeft().IsImm()) plan.right = {Operand::Kind::Imm, right.GetLeft().imm.Get(), 0};
    else if (right.GetLeft().IsValue()) plan.right = describe(right.GetLeft().value);
    if (plan.left.kind == Operand::Kind::None || plan.right.kind == Operand::Kind::None) return std::nullopt;
    const auto overlaps = [&](const Operand& operand, const ir::Uniform& uniform) {
        const u64 end = u64{uniform.GetOffset()} + ir::GetValueSizeByte(uniform.GetType());
        return operand.kind == Operand::Kind::Uniform && operand.value < end &&
               uniform.GetOffset() < operand.value + plan.width;
    };
    for (const auto& node : block.GetInstList()) {
        if (node.Id() <= producer->Id()) continue;
        if (node.GetOp() == ir::OpCode::StoreUniform) {
            const auto uniform = node.GetArg<ir::Uniform>(0);
            if (overlaps(plan.left, uniform) || overlaps(plan.right, uniform)) return std::nullopt;
        } else if (node.GetOp() == ir::OpCode::SetHostGPR) {
            const auto home = node.GetArg<ir::Imm>(1).Get();
            if ((plan.left.kind == Operand::Kind::HostGPR && plan.left.value == home) ||
                (plan.right.kind == Operand::Kind::HostGPR && plan.right.value == home)) return std::nullopt;
        }
    }
    return plan;
}
}
