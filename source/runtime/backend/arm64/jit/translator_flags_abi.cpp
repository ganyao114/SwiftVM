#include "translator.h"

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"
#include "runtime/common/svm_config.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::ParkFlagsHot() {
    if (!FlagsRegsEnabled()) {
        return;
    }
    MaterializeFlagsTokenResult();
    __ Mrs(ip1, NZCV);
    __ Orr(ip1, ip1, 1u << kFlagsNzcvParkValidBit);
    __ Str(ip1, MemOperand(state, state_offset_flags_nzcv_park));
    __ Str(atomic_scratch, MemOperand(state, state_offset_flags_result_park));
}

void JitTranslator::UnparkFlagsHot() {
    if (!FlagsRegsEnabled()) {
        return;
    }
    __ Msr(NZCV, flags);
}

EdgeFlagsState JitTranslator::PendingEdgeFlagsState(
        HostFlags valid,
        EdgeFlagsProducer producer) const {
    const auto mask = static_cast<u32>(valid);
    const auto carry_polarity = flag_state.edge_carry_source.Resolve(
            mask, CanonicalCarryEnabled());
    return EdgeFlagsState::Pending(mask,
                                   carry_polarity,
                                   producer);
}

void JitTranslator::ObserveEdgeCarryPolarity(ir::Uniform uniform,
                                              ir::Value value) {
    if (uniform.GetOffset() != offsetof(swift::x86::ThreadContext64,
                                        carry_inverted) ||
        uniform.GetType() != ir::ValueType::U8 ||
        value.Type() != ir::ValueType::U8) {
        return;
    }
    auto* definition = value.Def();
    if (!definition || definition->GetOp() != ir::OpCode::LoadImm) {
        flag_state.edge_carry_source.InvalidateRuntimePolarity();
        return;
    }
    const u64 raw = definition->GetArg<ir::Imm>(0).Get();
    if (raw > 1) {
        flag_state.edge_carry_source.InvalidateRuntimePolarity();
        return;
    }
    flag_state.edge_carry_source.PublishRuntimePolarity(raw != 0);
}

EdgeFlagsTargetContract JitTranslator::AnalyzeEdgeFlagsTarget(
        ir::Location target) const {
    const auto found = region_block_map.find(target.Value());
    if (found == region_block_map.end() || !found->second) {
        return {};
    }

    ir::Flags remaining = ir::Flags::NZCV;
    EdgeFlagsTargetContract contract{};
    auto overwrite = [&](ir::Flags flags_mask) {
        remaining &= ~flags_mask;
        contract.overwrite_before_observe = static_cast<u32>(
                GuestNZCVToHost(ir::Flags::NZCV & ~remaining));
    };
    auto observes = [&](ir::Flags flags_mask) {
        const auto observed = static_cast<u32>(GuestNZCVToHost(
                remaining & flags_mask & ir::Flags::NZCV));
        contract.observed_nzcv_mask |= static_cast<u8>(observed >> 28);
    };

    for (const auto& inst : found->second->GetInstList()) {
        const auto op = inst.GetOp();
        if (MayFaultOrObserve(inst)) {
            contract.barrier_before_commit = true;
            return contract;
        }
        switch (op) {
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
                observes(inst.GetArg<ir::Flags>(0));
                break;
            case ir::OpCode::GetFlags:
                observes(ir::Flags::NZCV);
                break;
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
                observes(ir::Flags::Carry);
                break;
            case ir::OpCode::InvertCarry:
                observes(ir::Flags::Carry);
                break;
            case ir::OpCode::CondSelect:
            case ir::OpCode::CondSet:
                observes(ir::Flags::NZCV);
                break;
            case ir::OpCode::LocalCondSet:
                observes(ir::Flags::NZCV);
                break;
            case ir::OpCode::Goto:
            case ir::OpCode::NotGoto:
            case ir::OpCode::BindLabel:
                contract.barrier_before_commit = true;
                return contract;
            case ir::OpCode::SaveFlags:
                overwrite(inst.GetArg<ir::Flags>(1));
                break;
            case ir::OpCode::ClearFlags:
                overwrite(inst.GetArg<ir::Flags>(0));
                break;
            case ir::OpCode::SetCarry:
                overwrite(ir::Flags::Carry);
                break;
            case ir::OpCode::SetOverflow:
                overwrite(ir::Flags::Overflow);
                break;
            case ir::OpCode::PublishFCmpFlags:
                overwrite(ir::Flags::NZCV);
                break;
            case ir::OpCode::BranchOnlyFlags:
                overwrite(ir::Flags::NZCV);
                break;
            case ir::OpCode::PublishSse42StrFlags:
                overwrite(inst.GetArg<ir::Flags>(1));
                break;
            case ir::OpCode::AdvancePC:
                contract.commits_before_fault =
                        contract.overwrite_before_observe != 0 &&
                        !contract.barrier_before_commit;
                return contract;
            default:
                break;
        }
    }
    return contract;
}

bool JitTranslator::TargetAcceptsEdgeFlags(
        ir::Location target,
        const EdgeFlagsState& incoming) const {
    return AnalyzeEdgeFlagsTarget(target).Accepts(incoming);
}

bool JitTranslator::TargetKillsIncomingFlags(ir::Location target) const {
    return TargetAcceptsEdgeFlags(
            target,
            PendingEdgeFlagsState(HostFlags::NZCV,
                                  EdgeFlagsProducer::Restore));
}

void JitTranslator::EmitFlagsPublishedVeneer(ir::Block* block) {
    if (!FlagsRegsEnabled() || !region_edges_active || !block) {
        return;
    }
    auto* published = context.GetLabel(block->GetStartLocation().Value());
    if (published->IsBound()) {
        return;
    }
    __ Bind(published);
    if (!TargetKillsIncomingFlags(block->GetStartLocation())) {
        UnparkFlagsHot();
    }
    // Land before the entry counter so L2 visits match FLAGS=0 published
    // entries. Internal taken edges still target the post-counter label.
    __ B(context.GetCountedEntryLabel(block->GetStartLocation().Value()));
}

}  // namespace swift::runtime::backend::arm64
