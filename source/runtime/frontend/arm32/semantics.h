#pragma once

#include "build_ir.h"
#include "instruction.h"
#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/service.h"
#include "runtime/arch/arm32/state.h"
#include "runtime/exit.h"
#include "runtime/frontend/guest_events.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace swift::arm32::detail {
using namespace runtime::ir;
using GuestState = runtime::arm32::GuestState;
using Numeric = runtime::arm32::Numeric;
using RoundingMode = runtime::arm32::RoundingMode;

struct Rejected {
    BuildError error;
    Error detail{Error::None};
};

enum class AccType { NORMAL, ATOMIC, ORDERED, UNPRIVILEGED, SWAP };
enum class Exception {
    UndefinedInstruction,
    UnpredictableInstruction,
    DecodeError,
    Breakpoint,
    WaitForEvent,
    WaitForInterrupt,
    Yield,
    SendEvent,
    SendEventLocal,
    PreloadData,
    PreloadDataWithIntentToWrite,
    PreloadInstruction
};

namespace Term {
struct LinkBlock {
    Location location;
};

using LinkBlockFast = LinkBlock;

struct ReturnToDispatch {};

struct PopRSBHint {};

struct FastDispatchHint {};

struct Interpret {
    Location location;
};

template <class T> struct CheckHalt {
    T next;
};

template <class Y, class N> struct If {
    Cond condition;
    Y yes;
    N no;
};

template <class Y, class N> struct CheckBit {
    Y yes;
    N no;
};
}  // namespace Term

using PopRSBHint = Term::PopRSBHint;

constexpr Type ScalarType(unsigned bits) {
    return bits == 1    ? Type::Bool
           : bits == 8  ? Type::U8
           : bits == 16 ? Type::U16
           : bits == 32 ? Type::U32
                        : Type::U64;
}

constexpr Type VectorType(unsigned bits) {
    return bits == 8    ? Type::V8
           : bits == 16 ? Type::V16
           : bits == 32 ? Type::V32
           : bits == 64 ? Type::V64
                        : Type::V128;
}

enum class ThreeRegsComparison {
    GE,
    GT,
    EQ,
    AbsoluteGE,
    AbsoluteGT,
};

template <class Sink> class Semantics final {
public:
    using NativeValue = typename Sink::Value;
    using Operand = typename Sink::Operand;

    struct Value {
        NativeValue raw{};
        Type view{Type::Void};
        OpCode origin{OpCode::Nop};
        u64 literal{};
        bool known{};

        Type ViewType() const { return view; }

        Value WithView(Type type) const {
            auto copy = *this;
            copy.view = type;
            return copy;
        }

        Value SetCastType(Type type) const { return WithView(type); }
    };

    struct Literal {
        u64 bits{};
        Type type{Type::U64};

        template <class T> explicit Literal(T value)
                : bits(static_cast<u64>(value))
                , type(sizeof(T) <= 1   ? Type::U8
                       : sizeof(T) <= 2 ? Type::U16
                       : sizeof(T) <= 4 ? Type::U32
                                        : Type::U64) {}

        u64 Get() const { return bits; }

        s64 GetSigned() const { return static_cast<s64>(bits); }
    };

    struct Uniform {
        u32 offset;
        Type type;
    };

    struct Input {
        Value value;
        Literal imm{u64{0}};
        bool immediate{};

        Input(Value value) : value(value) {}

        Input(Literal imm) : imm(imm), immediate(true) {}

        bool IsImm() const { return immediate; }

        bool IsValue() const { return !immediate; }

        Value GetValue() const { return value; }

        Literal GetImm() const { return imm; }
    };

    struct Control {
        enum class Kind { None, Jump, Conditional, Dispatch, Service };
        Kind kind{};
        Input destination{Literal{u64{0}}};
        Value condition;
        u64 yes{}, no{}, service{};
        bool predicated{};
    };

    struct ShiftResult {
        Value result, carry;
    };

    struct OverflowResult {
        Value result, overflow;
    };

    struct PackedResult {
        Value result, ge;
    };

    struct ImmAndCarry {
        u32 imm32;
        Value carry;
    };

    Semantics(Sink& sink, StateLayout layout, TargetInfo target, Profile profile = {})
            : sink(sink), layout(layout), target(target), profile(profile) {}

    void Execute(const Instruction& instruction, bool budget_safepoint = false) {
        current_pc = instruction.PC();
        current_location = Location{current_pc};
        if (current_location.EFlag()) throw Rejected{BuildError::Unsupported};
        current_word = instruction.SemanticWord();
        current_instruction_size = instruction.Length();
        next_location = Location{instruction.NextPC()};
        end_decode_ = false;
        control = {};
        pending_writeback.reset();
        SetLocation(current_pc);
        if (target.Has(TargetFeature::GuestEvents)) {
            using runtime::frontend::CrossesGuestEventWindow;
            using runtime::frontend::GuestEventTransfer;
            bool observe = budget_safepoint || CrossesGuestEventWindow(current_location.Address(),
                                                                       next_location.Address());
            bool dispatch_observe{};
            const auto flow = instruction.Flow();
            if (flow == FlowKind::Return)
                dispatch_observe = true;
            else if (flow == FlowKind::Jump || flow == FlowKind::Call ||
                     flow == FlowKind::Conditional) {
                u64 destination{};
                if (instruction.RelativeTarget(destination))
                    observe |= GuestEventTransfer(current_location.Address(),
                                                  Location{destination}.Address());
                else
                    dispatch_observe = true;
            }
            if (observe)
                Emit<OpCode::GuestSafepoint>(Literal{current_pc});
            else if (dispatch_observe)
                Emit<OpCode::GuestDispatchSafepoint>(Literal{current_pc});
        }
        const Cond condition = instruction.Condition();
        const bool predicated = condition != Cond::AL && condition != Cond::NV;
        Value predicate, guard;
        if (predicated) {
            predicate = CondPassed(condition);
            if constexpr (Sink::IsDirect) {
                if (!sink.Truthy(predicate.raw)) return;
            } else
                guard = Goto(IsZero(predicate));
        }
        if (!InstructionEnabled(instruction))
            UndefinedInstruction();
        else
            Dispatch(instruction);
        if (pending_writeback && control.kind != Control::Kind::Service)
            SetRegister(pending_writeback->reg, pending_writeback->value);
        if constexpr (!Sink::IsDirect) {
            if (predicated) {
                BindLabel(guard);
                if (control.kind == Control::Kind::Jump) {
                    control.kind = Control::Kind::Conditional;
                    control.condition = predicate;
                    control.no = next_location.Key();
                } else if (control.kind == Control::Kind::Dispatch) {
                    const auto destination = control.destination.IsImm()
                                                     ? Imm64(control.destination.imm.bits)
                                                     : control.destination.value;
                    control.destination =
                            ConditionalSelect(predicate, destination, Imm64(next_location.Key()));
                } else if (control.kind == Control::Kind::Service) {
                    control.predicated = true;
                    control.condition = predicate;
                    control.no = next_location.Key();
                }
            }
        }
    }

    Location NextLocation() const { return next_location; }

    Control control;
    u64 current_pc{};
    Location current_location;
    bool end_decode_{};

private:
    Sink& sink;
    StateLayout layout;
    TargetInfo target;
    Profile profile;
    Semantics& ir{*this};

    struct Options {
        bool hook_hint_instructions{}, define_unpredictable_behaviour{};
    } options;

    u32 current_word{};
    unsigned current_instruction_size{};
    Location next_location;
    Value check_bit;

    struct RegisterWriteback {
        Reg reg;
        Value value;
    };

    std::optional<RegisterWriteback> pending_writeback;

    template <class T>
        requires std::is_integral_v<T>
    Operand Arg(T value) const {
        return Operand::Immediate(static_cast<u64>(value), Type::U64);
    }

    Operand Arg(Operand value) const { return value; }

    Operand Arg(Value value) const { return Operand::ValueOf(value.raw, value.view); }

    Operand Arg(Literal value) const { return Operand::Immediate(value.bits, value.type); }

    Operand Arg(Uniform value) const { return Operand::Uniform(value.offset, value.type); }

    Operand Arg(Input value) const { return value.IsImm() ? Arg(value.imm) : Arg(value.value); }

    Operand Arg(Flag value) const { return Operand::Flags(value); }

    Operand Arg(Cond value) const { return Operand::Condition(value); }

    template <class T>
        requires std::is_integral_v<T>
    static Type Kind(T) {
        return Type::U64;
    }

    static Type Kind(Operand) { return Type::Void; }

    static Type Kind(Value value) { return value.view; }

    static Type Kind(Literal value) { return value.type; }

    static Type Kind(Uniform value) { return value.type; }

    static Type Kind(Input value) { return value.IsImm() ? Kind(value.imm) : Kind(value.value); }

    static Type Kind(Flag) { return Type::Void; }

    static Type Kind(Cond) { return Type::Void; }

    template <OpCode op, Type forced = Type::Void, class... Args> Value Emit(Args... input) {
        const std::array<Operand, sizeof...(Args)> args{Arg(input)...};
        const std::array<Type, sizeof...(Args)> types{Kind(input)...};
        Type type = forced;
        if constexpr (schemas[static_cast<std::size_t>(op)].result == ResultPolicy::Void)
            type = Type::Void;
        else if constexpr (schemas[static_cast<std::size_t>(op)].result == ResultPolicy::Fixed ||
                           schemas[static_cast<std::size_t>(op)].result ==
                                   ResultPolicy::IntegerFixed)
            type = schemas[static_cast<std::size_t>(op)].fixed_type;
        else if (type == Type::Void) {
            if constexpr (op == OpCode::VecSharedZero || op == OpCode::VecLoadConst ||
                          op == OpCode::VecDup64 || op == OpCode::CompareAndSwap128)
                type = Type::V128;
            else if constexpr (op == OpCode::GetUniformAddress || op == OpCode::VecExtract64 ||
                               op == OpCode::GetFlags || op == OpCode::ZeroExtend64 ||
                               op == OpCode::CountLeadingZeros64)
                type = Type::U64;
            else if constexpr (op == OpCode::CountLeadingZeros32)
                type = Type::U32;
            else if constexpr (op == OpCode::TestZero || op == OpCode::TestNotZero ||
                               op == OpCode::TestBit || op == OpCode::TestFlags ||
                               op == OpCode::TestNotFlags)
                type = Type::Bool;
            else if constexpr (op == OpCode::Select || op == OpCode::CondSelect)
                type = types[1];
            else if constexpr (sizeof...(Args))
                type = types[0];
        }
        const auto result = sink.template Emit<op>(type, std::span<const Operand>{args});
        if (!result) throw Rejected{BuildError::IRRejected, result.error};
        return {result.GetValue(), type, op};
    }

#define ARM32_IR_OP(name)                                                                          \
    template <Type type = Type::Void, class... A> Value name(A... args) {                          \
        return Emit<OpCode::name, type>(args...);                                                  \
    }
    ARM32_IR_OP(LoadUniform)
    ARM32_IR_OP(StoreUniform)
    ARM32_IR_OP(GetUniformAddress)
    ARM32_IR_OP(Add)
    ARM32_IR_OP(Sub)
    ARM32_IR_OP(Mul)
    ARM32_IR_OP(And)
    ARM32_IR_OP(Or)
    ARM32_IR_OP(Xor)
    ARM32_IR_OP(AndNot)
    ARM32_IR_OP(LslImm)
    ARM32_IR_OP(LsrImm)
    ARM32_IR_OP(AsrImm)
    ARM32_IR_OP(RorImm)
    ARM32_IR_OP(LslValue)
    ARM32_IR_OP(LsrValue)
    ARM32_IR_OP(AsrValue)
    ARM32_IR_OP(RorValue)
    ARM32_IR_OP(Select)
    ARM32_IR_OP(TestZero)
    ARM32_IR_OP(TestNotZero)
    ARM32_IR_OP(TestBit)
    ARM32_IR_OP(SetCarry)
    ARM32_IR_OP(SetOverflow)
    ARM32_IR_OP(VecExtract64)
    ARM32_IR_OP(VecDup64)
    ARM32_IR_OP(VecSharedZero)
#undef ARM32_IR_OP

    Value ImmValue(u64 value, Type type) {
        const auto result =
                sink.template Emit<OpCode::LoadImm>(type, {Operand::Immediate(value, type)});
        if (!result) throw Rejected{BuildError::IRRejected, result.error};
        return {result.GetValue(), type, OpCode::LoadImm, value, true};
    }

    Value Imm1(bool value) { return ImmValue(value, Type::Bool); }

    Value Imm8(u8 value) { return ImmValue(value, Type::U8); }

    Value Imm16(u16 value) { return ImmValue(value, Type::U16); }

    Value Imm32(u32 value) { return ImmValue(value, Type::U32); }

    Value Imm64(u64 value) { return ImmValue(value, Type::U64); }

    Value I(size_t bits, u64 value) { return ImmValue(value, ScalarType(bits)); }

    Value Eor(Value a, Value b) { return Xor(a, b); }

    Value Not(Value value) {
        return Xor(value, Literal{TypeBits(value.view) == 64 ? UINT64_MAX : UINT64_C(0xffffffff)});
    }

    Value IsZero(Value value) { return TestZero(value); }

    Value ConditionalSelect(Value condition, Value yes, Value no) {
        return Select(condition, yes, no);
    }

    Value CondPassed(Cond condition) { return Emit<OpCode::CondSet>(condition); }

    Value Goto(Value condition) {
        return {sink.LocalGoto(condition.raw, condition.view), condition.view, OpCode::Goto};
    }

    void BindLabel(Value label) { sink.LocalBind(label.raw); }

#include "decode/generated_dispatch.inc"
#include "semantics_control.inc"
#include "semantics_dsp.inc"
#include "semantics_integer.inc"
#include "semantics_memory.inc"
#include "semantics_multiply.inc"
#include "semantics_neon_float.inc"
#include "semantics_neon_integer.inc"
#include "semantics_neon_memory.inc"
#include "semantics_registers.inc"
#include "semantics_shift_flags.inc"
#include "semantics_system.inc"
#include "semantics_vfp.inc"
};
}  // namespace swift::arm32::detail
