#pragma once

#include "runtime/ir/instr.h"

namespace swift::runtime::compile {
// IR roles shared by the block summary and consumer candidate seeding. They
// describe semantic events, not target conditions or physical locations.
enum class ConsumerRole : u8 {
    None = 0,
    PairProducer = 1,
    PairSecondary = 2,
    CompoundClear = 4,
    LocalCondition = 8,
    PstateTest = 16,
    ReverseEvent = 32,
    AluAudit = 64,
    Entry = 128,
};
constexpr ConsumerRole operator|(ConsumerRole a, ConsumerRole b) {
    return static_cast<ConsumerRole>(static_cast<u8>(a) | static_cast<u8>(b));
}
constexpr bool HasRole(ConsumerRole roles, ConsumerRole role) {
    return (static_cast<u8>(roles) & static_cast<u8>(role)) != 0;
}
constexpr bool HasRole(u16 roles, ConsumerRole role) {
    return (roles & static_cast<u8>(role)) != 0;
}
constexpr ConsumerRole ConsumerRoles(ir::OpCode op) {
    using O = ir::OpCode;
    using R = ConsumerRole;
    switch (op) {
        case O::Div128:
        case O::Cpuid:
            return R::PairProducer | R::Entry;
        case O::Div128Remainder:
        case O::CpuidUpper:
            return R::PairSecondary | R::Entry;
        case O::ClearFlags:
            return R::CompoundClear | R::Entry;
        case O::CondSet:
        case O::TestZero:
        case O::TestNotZero:
        case O::FCmpCondSet:
        case O::LocalCondSet:
            return R::LocalCondition | R::ReverseEvent | R::Entry;
        case O::TestFlags:
        case O::TestNotFlags:
            return R::LocalCondition | R::PstateTest | R::ReverseEvent | R::Entry;
        case O::VecFCmp:
        case O::PublishFCmpFlags:
        case O::Sse42Str:
            return R::ReverseEvent | R::Entry;
        case O::And:
        case O::Or:
            return R::LocalCondition | R::AluAudit;
        case O::Sub:
        case O::Xor:
            return R::AluAudit;
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
        case O::CondSelect:
        case O::InvertCarry:
        case O::Adc:
        case O::Sbb:
        case O::PublishSse42StrFlags:
            return R::ReverseEvent;
        default:
            return R::None;
    }
}
}  // namespace swift::runtime::compile
