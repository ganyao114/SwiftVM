#pragma once

// Validate complete immediate/type contracts before allocation/dispatch.
#include <stdexcept>
#include "runtime/ir/instr.h"

namespace swift::runtime {
struct SaturatingAddSubShape { u32 bits; bool sign; };
// RV64 checks its existing V256 form with 32 before legalization, then all
// native backends check 16. A V256 view requires a truly wide producer;
// initialized scalar/narrow homes may be viewed up to the raw 128-bit extent.
inline SaturatingAddSubShape ValidateSaturatingAddSub(ir::Inst* inst, u32 container_bytes = 16) {
    if (!inst->ArgAt(2).IsImm() || !inst->ArgAt(3).IsImm())
        throw std::runtime_error("vector saturation requires immediate width and signed mode");
    const auto bits = inst->GetArg<ir::Imm>(2).Get(), mode = inst->GetArg<ir::Imm>(3).Get();
    if ((bits != 8 && bits != 16 && bits != 32 && bits != 64) || mode > 1)
        throw std::runtime_error("vector saturation requires exact width 8/16/32/64 and mode 0/1");
    const auto container = [=](ir::ValueType type) {
        return type != ir::ValueType::VOID && ir::GetValueSizeByte(type) <= container_bytes;
    };
    if (!container(inst->ReturnType())) throw std::runtime_error("vector saturation result exceeds raw 128-bit container");
    for (u32 index = 0; index < 2; ++index) {
        if (!inst->ArgAt(index).IsValue()) throw std::runtime_error("vector saturation requires defined inputs");
        const auto value = inst->GetArg<ir::Value>(index);
        if (!value.Defined() || !container(value.Type()) || !container(value.Def()->ReturnType()))
            throw std::runtime_error("vector saturation input exceeds raw 128-bit container");
        if (value.Type() == ir::ValueType::V256 && value.Def()->ReturnType() != ir::ValueType::V256)
            throw std::runtime_error("vector saturation V256 view requires an initialized wide producer");
    }
    return {u32(bits), mode == 1};
}
}  // namespace swift::runtime
