#pragma once
#include <optional>
#include "runtime/ir/uniform_effects.h"

namespace swift::runtime::ir {
inline std::optional<Condition> FlagCondition(std::uint64_t mask, bool inverse = false) {
    unsigned condition;
    switch (mask) {
        case 1:
            condition = unsigned(Condition::CS);
            break;
        case 2:
            condition = unsigned(Condition::VS);
            break;
        case 4:
            condition = unsigned(Condition::EQ);
            break;
        case 8:
            condition = unsigned(Condition::MI);
            break;
        default:
            return {};
    }
    return static_cast<Condition>(condition ^ unsigned(inverse));
}

inline unsigned ConditionFlags(Condition condition) {
    constexpr unsigned masks[]{4, 4, 1, 1, 8, 8, 2, 2, 5, 5, 10, 10, 14, 14, 0, 0};
    const auto value = unsigned(condition);
    return value < 16 ? masks[value] : 15;
}

inline std::uint64_t FlagReads(OpCode opcode,
                               Effect effects,
                               CellRange inputs,
                               const UniformPolicy& policy) {
    constexpr auto all = FlagBits(Flag::All);
    if (opcode == OpCode::VecLoadConst || opcode == OpCode::VecSharedZero) return 0;
    if (opcode == OpCode::ArmFPCompare && policy.flags) {
        auto at = inputs.begin();
        ++at;
        ++at;
        const auto offsets = *at;
        if (offsets.Kind() == OperandKind::Immediate &&
            !Overlaps({static_cast<std::uint32_t>(offsets.Payload()), 4}, *policy.flags) &&
            !Overlaps({static_cast<std::uint32_t>(offsets.Payload() >> 32), 4}, *policy.flags))
            return 0;
    }
    if (HasAnyEffect(effects,
                     Effect::MayFault | Effect::Unknown | Effect::External | Effect::Control))
        return all;
    if (HasAnyEffect(effects, Effect::ReadGuestState | Effect::WriteGuestState)) {
        if (!policy.flags) return all;
        Operation operation{};
        operation.opcode = opcode;
        const auto accesses = UniformEffectsFor(operation, inputs, policy);
        if (accesses.all_read || accesses.all_write) return all;
        for (unsigned i = 0; i < accesses.count; ++i)
            if (Overlaps(accesses.fields[i].range, *policy.flags)) return all;
    }
    if (!HasAnyEffect(effects, Effect::ReadFlags)) return 0;
    switch (opcode) {
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::InvertCarry:
            return 1;
        case OpCode::TestFlags:
        case OpCode::TestNotFlags: {
            const auto flags = inputs.front();
            return flags.Kind() == OperandKind::Flags && !(flags.Payload() & ~all) ? flags.Payload()
                                                                                   : all;
        }
        case OpCode::CondSet:
        case OpCode::LocalCondSet:
        case OpCode::CondSelect: {
            const auto condition = inputs.front();
            return condition.Kind() == OperandKind::Condition
                           ? ConditionFlags(static_cast<Condition>(condition.Payload()))
                           : all;
        }
        default:
            return all;
    }
}
}  // namespace swift::runtime::ir
