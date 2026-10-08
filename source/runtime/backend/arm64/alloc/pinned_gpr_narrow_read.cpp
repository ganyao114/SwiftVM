#include "pinned_gpr_allocation.h"

namespace swift::runtime::backend::arm64 {

std::optional<u16> MatchNarrowFixedGPRRead(
        ir::Inst& read, const BlockAnalysisIndex& block_analysis,
        const GuestStateMap& guest_state_map,
        const std::function<ir::Flags(ir::Inst*)>& set_flags) {
    if (read.GetOp() != ir::OpCode::GetHostGPR) return std::nullopt;
    const u32 home = read.GetArg<ir::Imm>(0).Get();
    const u32 width = ir::GetValueSizeByte(read.ReturnType());
    if (read.GetArg<ir::Imm>(1).Get() != 0 || !IsFixedGPRHome(home) ||
        read.GetUses() == 0 || width > sizeof(u32)) {
        return std::nullopt;
    }
    const auto uses = block_analysis.Uses(&read);
    if (uses.size() != 1 || uses.front().count != read.GetUses()) return std::nullopt;
    auto* consumer = uses.front().consumer;
    for (auto value : consumer->GetValues()) {
        if (value.Def() == &read && ir::GetValueSizeByte(value.Type()) > width)
            return std::nullopt;  // A dirty X register cannot supply a widened W capture.
    }
    if (consumer->Id() <= read.Id() ||
        !guest_state_map.FixedHomeSurvives(home, read.Id(), consumer->Id())) {
        return std::nullopt;
    }
    const auto op = consumer->GetOp();
    const u32 result_width = consumer->HasValue() && consumer->ReturnType() != ir::ValueType::VOID
            ? ir::GetValueSizeByte(consumer->ReturnType()) : 0;
    const bool direct_alu =
            (op == ir::OpCode::And || op == ir::OpCode::Xor) &&
            result_width <= sizeof(u32) &&
            (home <= 5 || width == sizeof(u32) ||
             (home >= 22 && !X86PinExtLevel3Requested()));
    const bool direct_or = op == ir::OpCode::Or &&
            width == sizeof(u32) && result_width == sizeof(u32);
    const bool add_sub = op == ir::OpCode::Add || op == ir::OpCode::Sub;
    const bool caller_alu = home <= 9 && add_sub &&
            result_width <= sizeof(u32) && (home <= 5 || width == sizeof(u32));
    const bool adjacent_flags = home >= 6 && home <= 9 &&
            (width == sizeof(u8) || width == sizeof(u16)) &&
            consumer->Id() == read.Id() + 1 && add_sub && result_width == width &&
            True(set_flags(consumer) & ir::Flags::NZCV);
    const bool callee_sub = home >= 19 && op == ir::OpCode::Sub &&
            result_width <= sizeof(u32);
    const bool zero_extend = (width == sizeof(u8) || width == sizeof(u16)) &&
            op == ir::OpCode::ZeroExtend32 && consumer->GetArg<ir::Value>(0).Def() == &read;
    const bool sign_extend = home >= 19 && op == ir::OpCode::SignExtend &&
            consumer->GetArg<ir::Value>(0).Def() == &read;
    const bool store = uses.front().count == 1 && op == ir::OpCode::StoreMemory &&
            consumer->GetArg<ir::Value>(1).Def() == &read;
    return direct_alu || direct_or || caller_alu || adjacent_flags || callee_sub ||
                   zero_extend || sign_extend || store
            ? std::optional<u16>{static_cast<u16>(home)} : std::nullopt;
}

void PinnedGPRAllocation::PrepareNarrowReads(
        const std::function<ir::Flags(ir::Inst*)>& set_flags) {
    for (auto& read : block->GetInstList()) {
        if (read.GetOp() != ir::OpCode::GetHostGPR ||
            allocation.IsFixedGPRDefinitionElided(read.Id()) ||
            allocation.IsHostReadCoalesced(read.Id()) || HasLowView(&read) ||
            ValueFullyResident(&read)) continue;
        if (const auto home = MatchNarrowFixedGPRRead(
                    read, block_analysis, guest_state_map, set_flags)) {
            AssignLowView(&read, *home);
        }
    }
}

}  // namespace swift::runtime::backend::arm64
