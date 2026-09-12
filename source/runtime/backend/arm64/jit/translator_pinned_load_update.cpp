#include "runtime/backend/reg_alloc.h"
#include "translator.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

ir::Value ResolveBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

std::optional<s64> ImmediateValue(const ir::Operand& operand) {
    if (operand.GetRight().IsImm()) {
        return operand.GetRight().imm.GetSigned();
    }
    if (operand.IsImm()) {
        return operand.GetLeft().imm.GetSigned();
    }
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    auto value = ResolveBitCast(operand.GetLeft().value);
    if (!value.Def() || value.Def()->GetOp() != ir::OpCode::LoadImm) {
        return std::nullopt;
    }
    return static_cast<s64>(value.Def()->GetArg<ir::Imm>(0).Get());
}

}  // namespace

std::optional<JitTranslator::PinnedLoadUpdate>
JitTranslator::MatchPinnedLoadUpdate(ir::Inst* update) {
    if (memory_state.use_memory_base || !update || update->GetOp() != ir::OpCode::Add ||
        update->ReturnType() != ir::ValueType::U64 ||
        !GetPseudoFlags(update).Null()) {
        return std::nullopt;
    }
    auto base = ResolveBitCast(update->GetArg<ir::Value>(0));
    auto* base_read = base.Def();
    const auto offset = ImmediateValue(update->GetArg<ir::Operand>(1));
    if (!base_read || base_read->GetOp() != ir::OpCode::GetHostGPR ||
        base_read->GetArg<ir::Imm>(1).Get() != 0 || !offset ||
        *offset != 1) {
        return std::nullopt;
    }
    const u32 target = base_read->GetArg<ir::Imm>(0).Get();
    if (!IsFixedGPRHome(target) || base_read->GetUses(false) != 2) {
        return std::nullopt;
    }

    ir::Inst* load{};
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() >= update->Id()) {
            break;
        }
        if (scan.GetOp() != ir::OpCode::LoadMemory) {
            continue;
        }
        const auto address = scan.GetArg<ir::Operand>(0);
        if (address.GetOp() != ir::OperandOp::Plus ||
            !address.GetLeft().IsValue() ||
            ResolveBitCast(address.GetLeft().value).Def() != base_read) {
            continue;
        }
        const auto memory_offset = ImmediateValue(address);
        if (memory_offset && *memory_offset == *offset) {
            load = &scan;
        }
    }
    ir::Inst* publication{};
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= update->Id()) {
            continue;
        }
        u32 uses{};
        for (auto value : scan.GetValues()) {
            uses += value.Def() == update;
        }
        if (!uses) {
            continue;
        }
        if (uses == 1 && scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Value>(0).Def() == update &&
            scan.GetArg<ir::Imm>(1).Get() == target &&
            scan.GetArg<ir::Imm>(2).Get() == 0) {
            publication = &scan;
        }
        break;
    }
    if (!load || !publication || load->Id() >= update->Id() ||
        context.X(ir::Value{load}).GetCode() == target) {
        return std::nullopt;
    }
    u32 ordinary_uses{};
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.GetOp() == ir::OpCode::SaveFlags ||
            scan.GetOp() == ir::OpCode::BranchOnlyFlags ||
            scan.GetOp() == ir::OpCode::ClearFlags) {
            continue;
        }
        for (auto value : scan.GetValues()) {
            ordinary_uses += value.Def() == update;
        }
    }
    if (ordinary_uses != 1) {
        return std::nullopt;
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= load->Id() || scan.Id() >= update->Id()) {
            continue;
        }
        if (MayFaultOrObserve(scan) ||
            (scan.GetOp() == ir::OpCode::GetHostGPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return std::nullopt;
        }
    }
    return PinnedLoadUpdate{
            .load = load,
            .update = update,
            .publication = publication,
            .base_read = base_read,
            .target = static_cast<u16>(target),
            .offset = static_cast<s16>(*offset),
    };
}

void JitTranslator::PreparePinnedLoadUpdates(ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchPinnedLoadUpdate(&inst);
        if (!candidate) {
            continue;
        }
        pinned_gprs.fused_pin_gpr_reads.emplace(candidate->base_read, candidate->target);
        pinned_gprs.pinned_load_updates.emplace(candidate->load, *candidate);
        pinned_gprs.pinned_load_update_instructions.emplace(candidate->update, *candidate);
        pinned_gprs.pinned_load_update_instructions.emplace(candidate->publication, *candidate);
    }
}

}  // namespace swift::runtime::backend::arm64
