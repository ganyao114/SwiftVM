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

EdgeFlagsTargetContract JitTranslator::AnalyzeEdgeFlagsTarget(ir::Location target) const {
    if (RequiresCanonicalBackedgeEntry(target)) return {};
    const auto* found = Edges().InternalBlock(target.Value());
    return found == nullptr ? EdgeFlagsTargetContract{} : found->edge_flags;
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

void JitTranslator::EmitFlagsPublishedVeneer(const ir::Block* block) {
    if (!FlagsRegsEnabled() || !RegionEdgesActive() || !block) {
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
