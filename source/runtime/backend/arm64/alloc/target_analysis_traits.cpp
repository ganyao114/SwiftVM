#include "target_analysis_traits.h"
#include "consumer_plan.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/jit/guest_state_map.h"
#include "runtime/backend/arm64/helper_call_contract.h"
namespace swift::runtime::backend::arm64 {
bool TargetPreservesHostNZCV(ir::OpCode op) {
    // Deliberately narrow emitter audit. These are the only pre-producer
    // operations admitted by the first spike; every listed ARM64 lowering is
    // flag-neutral (including its address arithmetic and CBNZ/TBZ guards).
    switch (op) {
        case ir::OpCode::LoadUniform:
        case ir::OpCode::StoreUniform:
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::GetHostGPR:
        case ir::OpCode::GetHostFPR:
        case ir::OpCode::SetHostGPR:
        case ir::OpCode::SetHostFPR:
        case ir::OpCode::LoadImm:
        case ir::OpCode::AdvancePC:
        case ir::OpCode::BitCast:
        case ir::OpCode::GetOperand:
        case ir::OpCode::Zero:
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::ZeroExtend32To64:
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        // These lower to their non-S forms when no surviving SaveFlags pseudo
        // names them. The proof stops at the first producer that does have
        // such a pseudo, so earlier dead-flags pointer arithmetic is neutral.
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
            return true;
        default:
            return false;
    }
}


TargetWindowEffects AnalyzeTargetWindowEffects(const ir::Inst& inst,
        const FeatureSet& features, bool needs_nzcv, std::optional<u16> home) {
    const auto helper = HelperCallContract::Resolve(inst, features);
    TargetWindowEffects facts;
    if (needs_nzcv) {
        facts.retains_nzcv = helper ? helper->RetainsPendingNZCV()
                                   : TargetPreservesHostNZCV(inst.GetOp());
        // GuestStateMap's instruction predicate has exactly this feature-bound
        // helper rule plus its shared pure opcode effects table.
        facts.observes_guest = helper ? helper->RequiresGuestStatePublication()
                                     : TargetMayFaultOrObserve(inst.GetOp());
    }
    if (home)
        facts.clobbers_home = (FixedGPRClobbers(inst, features) & (1u << *home)) ||
                             (helper && helper->ClobbersGPR(*home));
    return facts;
}

bool TargetRetainsPendingHostNZCV(const ir::Inst& inst, const FeatureSet& features) {
    const auto helper = HelperCallContract::Resolve(inst, features);
    return helper ? helper->RetainsPendingNZCV() : TargetPreservesHostNZCV(inst.GetOp());
}
bool TargetMayFaultOrObserve(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::CheckMemoryAlignment:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
        case ir::OpCode::Sse42Str:
            return true;
        default:
            return false;
    }
}
bool TargetMayFaultOrObserve(const ir::Inst& inst, const FeatureSet& features) {
    const auto helper = HelperCallContract::Resolve(inst, features);
    return helper ? helper->RequiresGuestStatePublication()
                  : TargetMayFaultOrObserve(inst.GetOp());
}

bool TargetMayFaultOrObserve(const ir::Inst& inst, const GuestStateMap& facts) {
    return facts.MayFaultOrObserve(inst);
}
bool TargetCanUseZeroStoreRegister(ir::Value value, const RegAlloc& allocation,
                                  const FeatureSet& features, const ConsumerPlan* consumers) {
    return features.zero_store_zr && value.Defined() &&
           (allocation.ZeroStoreVerified(value.Id()) ||
            (consumers && consumers->incoming_zeros.contains(value.Def()))) &&
           !ir::IsFloatValueType(value.Type()) && ir::GetValueSizeByte(value.Type()) <= sizeof(u64);
}
} // namespace swift::runtime::backend::arm64
