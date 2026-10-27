#pragma once
#include "runtime/compile/allocate/allocation.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::arm64 {
class UnitActions;

enum class SpillForwardConsumerKind : u8 { Ordinary, Portable, Barrier, FlagsOnly };

// Opcode-only qualification is frozen during allocation, before emission.
[[nodiscard]] constexpr SpillForwardConsumerKind ClassifySpillForwardConsumer(
        ir::OpCode op) noexcept {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadMemory:
        case O::StoreMemory:
        case O::LoadMemoryTSO:
        case O::StoreMemoryTSO:
        case O::MemoryCopy:
        case O::MemoryCopyTSO:
        case O::CompareAndSwap:
        case O::CompareAndSwap128:
        case O::CheckMemoryAlignment:
        case O::AtomicExchange:
        case O::AtomicFetchAdd:
        case O::AtomicRMW:
        case O::CallLambda:
        case O::CallLocation:
        case O::CallDynamic:
        case O::X87Op:
        case O::Sse42Str:
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
            return SpillForwardConsumerKind::Barrier;
        case O::Add:
        case O::Sub:
        case O::Adc:
        case O::Sbb:
        case O::And:
        case O::Or:
        case O::Xor:
        case O::Not:
        case O::Select:
        case O::SelectZero:
        case O::CondSelect:
        case O::LslImm:
        case O::LsrImm:
        case O::AsrImm:
        case O::LslValue:
        case O::LsrValue:
        case O::AsrValue:
        case O::GetOperand:
        case O::SetHostGPR:
        case O::StoreUniform:
            return SpillForwardConsumerKind::Portable;
        case O::SaveFlags:
        case O::BranchOnlyFlags:
            return SpillForwardConsumerKind::FlagsOnly;
        default:
            return SpillForwardConsumerKind::Ordinary;
    }
}

struct TargetWindowEffects {
    bool retains_nzcv{true};
    bool observes_guest{};
    bool clobbers_home{};
};
// Resolve the helper contract once for the requirements of this proof point.
TargetWindowEffects AnalyzeTargetWindowEffects(const ir::Inst& inst,
                                               const FeatureSet& features,
                                               bool needs_nzcv,
                                               std::optional<u16> home);
bool TargetPreservesHostNZCV(ir::OpCode op);
bool TargetRetainsPendingHostNZCV(const ir::Inst& inst, const FeatureSet& features);
bool TargetMayFaultOrObserve(ir::OpCode op);
bool TargetMayFaultOrObserve(const ir::Inst& inst, const FeatureSet& features);
bool TargetCanUseZeroStoreRegister(ir::Value value,
                                   const ir::Inst* consumer,
                                   const UnitActions& actions,
                                   const FeatureSet& features);

// Ordinary SSA aliases have no consumer-dependent preservation override.
inline bool SharesAllocatedGPR(compile::AllocationView allocation,
                               ir::Value left,
                               ir::Value right) {
    return allocation.ValueType(left) == compile::Allocation::GPR &&
           allocation.ValueType(right) == compile::Allocation::GPR &&
           allocation.ValueGPR(left).id == allocation.ValueGPR(right).id;
}
}  // namespace swift::runtime::backend::arm64
