#pragma once

#include <stdexcept>
#include "runtime/ir/instr.h"
#include "runtime/ir/block.h"
#include "native_call_arity.h"
#include "condition_emission_contract.h"
#include "scalar_uniform_emission_contract.h"

namespace swift::runtime::backend::riscv64 {

// Secondary pair markers are supplied only by their designated producer.
// This checks that relation; it does not add ordering/width/duplicate policy.
inline void VerifyPairPseudoProducerForOpcode(const ir::Inst& inst, ir::OpCode op) {
    ir::OpCode expected;
    switch (op) {
        case ir::OpCode::CpuidUpper: expected = ir::OpCode::Cpuid; break;
        case ir::OpCode::Div128Remainder: expected = ir::OpCode::Div128; break;
        default: return;
    }
    if (!inst.ArgAt(0).IsValue())
        throw std::runtime_error("RV64 secondary pair marker has no value producer");
    const auto value = inst.GetArg<ir::Value>(0);
    if (!value.Defined() || value.Def()->GetOp() != expected)
        throw std::runtime_error("RV64 secondary pair marker has the wrong producer");
}

inline void VerifyPairPseudoProducer(const ir::Inst& inst) {
    VerifyPairPseudoProducerForOpcode(inst, inst.GetOp());
}

// Share the opcode query for these two RV-only call contracts. The
// selected verifier retains its complete validation and rejection policy.
template <bool emission, bool uniform_bounds>
[[gnu::always_inline]] inline void VerifyNativeCallContractsCore(const ir::Inst& inst, u32 capacity, const ir::Block* block) {
    const auto op = inst.GetOp();
    switch (op) {
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
            VerifyNativeCallArityForOpcode(inst, op);
            return;
        case ir::OpCode::CpuidUpper:
        case ir::OpCode::Div128Remainder:
            VerifyPairPseudoProducerForOpcode(inst, op);
            if constexpr (emission) {
                // Check the final marker set immediately before emission.
                // Prepare may remove dead markers; do not reject them earlier.
                if (!block)
                    throw std::runtime_error("RV64 emission contract check has no block");
                if (ir::CollectPseudoOperations(*block, inst.GetArg<ir::Value>(0).Def(), op).size() > 1)
                    throw std::runtime_error("RV64 paired producer has duplicate secondary markers");
            }
            return;
        case ir::OpCode::CondSet:
        case ir::OpCode::LocalCondSet:
        case ir::OpCode::CondSelect:
        case ir::OpCode::FCmpCondSet:
            if constexpr (emission) VerifyRVInstructionConditionForOpcode(inst, op);
            return;
        case ir::OpCode::LoadUniform:
        case ir::OpCode::StoreUniform:
            if constexpr (emission && uniform_bounds) VerifyRVScalarUniformSpanForOpcode(inst, op, capacity);
            return;
        default:
            return;
    }
}

// Preserve the old template and all unique single-argument public wrappers.
template<bool emission>
[[gnu::always_inline]] inline void VerifyNativeCallContractsImpl(const ir::Inst& inst, const ir::Block* block = nullptr) {
    VerifyNativeCallContractsCore<emission, false>(inst, 0, block);
}

[[gnu::always_inline]] inline void VerifyNativeCallContractsForConfiguredEmission(const ir::Inst& inst, u32 capacity, const ir::Block* block) {
    VerifyNativeCallContractsCore<true, true>(inst, capacity, block);
}

// Preserve the existing unique single-argument API and Prepare contract.
[[gnu::always_inline]] inline void VerifyNativeCallContracts(const ir::Inst& inst) {
    VerifyNativeCallContractsImpl<false>(inst);
}

[[gnu::always_inline]] inline void VerifyNativeCallContractsForEmission(const ir::Inst& inst, const ir::Block* block) {
    VerifyNativeCallContractsImpl<true>(inst, block);
}

} // namespace swift::runtime::backend::riscv64
