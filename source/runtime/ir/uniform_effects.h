#pragma once

#include <array>
#include "optimize.h"

namespace swift::runtime::ir {
struct UniformAccess {
    UniformRange range;
    bool read{}, write{};
};

struct UniformEffects {
    std::array<UniformAccess, 4> fields{};
    unsigned count{};
    bool all_read{}, all_write{};
};

inline bool Overlaps(UniformRange left, UniformRange right) {
    return std::uint64_t{left.offset} < std::uint64_t{right.offset} + right.bytes &&
           std::uint64_t{right.offset} < std::uint64_t{left.offset} + left.bytes;
}

// Architectural state effects are independent of the host registers used by
// an emitter. Unknown addresses and helpers retain their full observation.
inline UniformEffects UniformEffectsFor(const Operation& op,
                                        CellRange inputs,
                                        const UniformPolicy& policy) {
    UniformEffects result;
    const auto effects = EffectsFor(op.opcode, op.Metadata());
    result.all_read = HasAnyEffect(effects, Effect::ReadGuestState);
    result.all_write = HasAnyEffect(effects, Effect::WriteGuestState);
    const auto add = [&](UniformRange range, bool read, bool write) {
        result.fields[result.count++] = {range, read, write};
    };
    if (op.opcode == OpCode::LoadUniform || op.opcode == OpCode::StoreUniform ||
        op.opcode == OpCode::StoreUniformSelectImm) {
        const auto field = inputs.front();
        const auto bytes = (TypeBits(field.TypeView()) + 7) / 8;
        if (field.Kind() == OperandKind::Uniform && field.Payload() <= UINT32_MAX && bytes) {
            add({static_cast<std::uint32_t>(field.Payload()), bytes},
                op.opcode == OpCode::LoadUniform,
                op.opcode != OpCode::LoadUniform);
            result.all_read = result.all_write = false;
        }
    } else if (op.opcode == OpCode::ArmFPBinary) {
        auto at = inputs.begin();
        ++at;
        ++at;
        const auto control = *at++, status = *at;
        if (control.Kind() == OperandKind::Immediate && status.Kind() == OperandKind::Immediate &&
            control.Payload() <= UINT32_MAX - 4 && status.Payload() <= UINT32_MAX - 4) {
            add({static_cast<std::uint32_t>(control.Payload()), 4}, true, false);
            add({static_cast<std::uint32_t>(status.Payload()), 4}, true, true);
            result.all_read = result.all_write = false;
        }
    } else if (policy.pc && (op.opcode == OpCode::AdvancePC || op.opcode == OpCode::SetLocation ||
                             op.opcode == OpCode::GetLocation)) {
        add(*policy.pc, op.opcode != OpCode::SetLocation, op.opcode != OpCode::GetLocation);
        result.all_read = result.all_write = false;
    }
    if (HasAnyEffect(effects, Effect::ReadFlags | Effect::WriteFlags)) {
        const bool write = HasAnyEffect(effects, Effect::WriteFlags);
        if (policy.flags)
            add(*policy.flags, true, write);
        else {
            result.all_read = true;
            result.all_write |= write;
        }
    }
    if (op.opcode != OpCode::ArmFPBinary &&
        HasAnyEffect(effects, Effect::ReadHostState | Effect::WriteHostState)) {
        const bool write = HasAnyEffect(effects, Effect::WriteHostState);
        if (policy.numeric_control)
            add(*policy.numeric_control, true, write);
        else {
            result.all_read = true;
            result.all_write |= write;
        }
    }
    return result;
}
}  // namespace swift::runtime::ir
