#pragma once

#include "runtime/ir/instr.h"

namespace swift::runtime::backend::x64 {

struct ProducerFlagsUsage {
    u64 published_mask{};
    bool needs_metadata{}, needs_result{}, nz_from_native{};
};

// The producer's canonical return width governs SaveFlags, independently of
// the marker's Value cast. Result homes and native snapshot homes are separate.
inline ProducerFlagsUsage ClassifyProducerFlags(ir::OpCode op, ir::ValueType type, ir::Flags requested) {
    using O = ir::OpCode;
    using T = ir::ValueType;
    if (type < T::U8 || type > T::S64) return {};
    const auto wants = [&](ir::Flags flag) { return True(requested & flag); };
    const bool arithmetic = op == O::Add || op == O::Sub || op == O::Adc || op == O::Sbb || op == O::Neg;
    const bool logical = op == O::And || op == O::Or || op == O::Xor || op == O::AndNot;
    const bool multiply = op == O::Mul && ir::GetValueSizeByte(type) < 8;
    ProducerFlagsUsage usage;
    if (wants(ir::Flags::Negate)) usage.published_mask |= u64{1} << 31;
    if (wants(ir::Flags::Zero)) usage.published_mask |= u64{1} << 30;
    if (arithmetic || logical || multiply) {
        if (wants(ir::Flags::Carry)) usage.published_mask |= u64{1} << 29;
        if (wants(ir::Flags::Overflow)) usage.published_mask |= u64{1} << 28;
    }
    if (arithmetic && wants(ir::Flags::AuxiliaryCarry)) usage.published_mask |= u64{1} << 26;
    if (wants(ir::Flags::Parity)) usage.published_mask |= 255;
    usage.needs_metadata = ((arithmetic || multiply) && wants(ir::Flags::CV)) ||
                           (arithmetic && wants(ir::Flags::AuxiliaryCarry));
    usage.nz_from_native = arithmetic && usage.needs_metadata;
    usage.needs_result = wants(ir::Flags::Parity) || (wants(ir::Flags::NZ) && !usage.nz_from_native);
    return usage;
}

} // namespace swift::runtime::backend::x64
