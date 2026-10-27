#include "effects.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/compile/register_contract.h"
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

namespace {
bool RetainsPendingNZCVWithoutHelper(const ir::Inst& inst) {
    using ir::OpCode;
    using ir::ValueType;
    switch (inst.GetOp()) {
        case OpCode::LoadMemoryTSO:
            // V128 uses LDR Q plus DMB ISHLD; its address arithmetic does
            // not set NZCV. It remains faultable for precise recovery.
            return inst.ReturnType() == ValueType::V128;
        case OpCode::VecAnd:
        case OpCode::VecAndNot:
        case OpCode::VecOr:
        case OpCode::VecXor:
            return inst.ReturnType() == ValueType::V128 &&
                   inst.GetArg<ir::Value>(0).Type() == ValueType::V128 &&
                   inst.GetArg<ir::Value>(1).Type() == ValueType::V128;
        case OpCode::VecFCmpMask: {
            const auto bits = inst.GetArg<ir::Imm>(2).Get();
            return inst.ReturnType() == ValueType::V128 &&
                   inst.GetArg<ir::Value>(0).Type() == ValueType::V128 &&
                   inst.GetArg<ir::Value>(1).Type() == ValueType::V128 &&
                   (bits == 32 || bits == 64) && inst.GetArg<ir::Imm>(4).Get() == 0;
        }
        default:
            return TargetPreservesHostNZCV(inst.GetOp());
    }
}
}  // namespace

TargetWindowEffects AnalyzeTargetWindowEffects(const ir::Inst& inst,
                                               const FeatureSet& features,
                                               bool needs_nzcv,
                                               std::optional<u16> home) {
    const auto helper = HelperCallContract::Resolve(inst, features);
    TargetWindowEffects facts;
    if (needs_nzcv) {
        facts.retains_nzcv =
                helper ? helper->RetainsPendingNZCV() : RetainsPendingNZCVWithoutHelper(inst);
        // Use the feature-bound helper rule and shared pure opcode table.
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
    return helper ? helper->RetainsPendingNZCV() : RetainsPendingNZCVWithoutHelper(inst);
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
    return helper ? helper->RequiresGuestStatePublication() : TargetMayFaultOrObserve(inst.GetOp());
}

bool TargetCanUseZeroStoreRegister(ir::Value value,
                                   const ir::Inst* consumer,
                                   const UnitActions& actions,
                                   const FeatureSet& features) {
    if (!features.zero_store_zr || !value.Defined() || ir::IsFloatValueType(value.Type()) ||
        ir::GetValueSizeByte(value.Type()) > sizeof(u64))
        return false;
    if (actions.ZeroStoreVerified(value.Id())) return true;
    const auto* row = consumer ? actions.Consumer(consumer->Id()) : nullptr;
    return row && row->MatchesZeroInput(*consumer, value);
}

}  // namespace swift::runtime::backend::arm64
