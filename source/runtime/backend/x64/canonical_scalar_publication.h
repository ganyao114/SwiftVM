#pragma once

#include "runtime/ir/instr.h"

namespace swift::runtime::backend::x64 {

// This positive inventory describes the current x64 lowering, not scalar IR
// annotations in general. Every accepted path publishes through Write, which
// clears bits above ReturnType before installing the assigned register.
inline bool HasCanonicalScalarPublication(const ir::Inst* producer) {
    if (!producer) return false;
    const auto type = producer->ReturnType();
    if (type < ir::ValueType::U8 || type > ir::ValueType::S64) return false;
    using O = ir::OpCode;
    switch (producer->GetOp()) {
        case O::LoadUniform: {
            // The Uniform's width selects Write versus WriteVector. A narrow
            // result annotation on a 128-bit load is insufficient evidence.
            const auto size = ir::GetValueSizeByte(producer->GetArg<ir::Uniform>(0).GetType());
            return size == 1 || size == 2 || size == 4 || size == 8;
        }
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::And: case O::Or: case O::Xor: case O::AndNot:
        case O::Neg:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64:
        case O::SignExtend:
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
        case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
        case O::ByteSwap: case O::BitExtract: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return true;
        // Phi, raw/vector aliases, helpers, LoadImm/rematerialization and
        // pseudo publications need separate proofs before joining this list.
        default: return false;
    }
}

} // namespace swift::runtime::backend::x64
