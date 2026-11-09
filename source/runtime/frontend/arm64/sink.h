#pragma once
#include "build_ir.h"
#include "interpreter.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/exit.h"
#include "runtime/ir/direct/interpreter.h"
#include "semantics.h"

namespace swift::arm64::detail {
namespace direct = runtime::ir::direct;

class IRSink : public runtime::ir::Builder {
public:
    using Value = runtime::ir::Value;
    using Operand = runtime::ir::Operand;
    using Producer = runtime::ir::NodeId;

    IRSink(runtime::ir::Scope scope, runtime::ir::TargetInfo target, runtime::ir::Program reuse)
            : Builder(scope, target, std::move(reuse)) {}

    Producer ProducerOf(Value v) const { return v.node; }

    runtime::ir::OperationMetadata Metadata(Producer node) const {
        return Operations()[node.value].Metadata();
    }

    Value LocalGoto(Value condition, runtime::ir::Type view) {
        const auto out = Emit<runtime::ir::OpCode::Goto>(view, {Operand::ValueOf(condition, view)});
        if (!out) throw Rejected{BuildError::IRRejected, out.error};
        return out.GetValue();
    }

    void LocalBind(Value label) {
        const auto r = Emit<runtime::ir::OpCode::BindLabel>(runtime::ir::Type::Void,
                                                            {Operand::ValueOf(label)});
        if (!r) throw Rejected{BuildError::IRRejected, r.error};
    }

    Value Helper(runtime::arm64::Numeric family,
                 std::uint32_t word,
                 std::uint64_t pc,
                 std::uint32_t extra,
                 std::uint32_t state_offset) {
        using namespace runtime::ir;
        const auto value = [](const auto& result) {
            if (!result) throw Rejected{BuildError::IRRejected, result.error};
            return result.GetValue();
        };
        const auto pointer = value(Emit<OpCode::GetUniformAddress>(
                Type::U64, {Operand::Immediate(state_offset, Type::U32)}));
        const auto operation = value(Emit<OpCode::LoadImm>(
                Type::U64,
                {Operand::Immediate(runtime::arm64::NumericCall(family, word, extra), Type::U64)}));
        const auto location =
                value(Emit<OpCode::LoadImm>(Type::U64, {Operand::Immediate(pc, Type::U64)}));
        const auto result = Emit<OpCode::CallLambda>(
                Type::U64,
                {Operand::Lambda(),
                 Operand::Immediate(
                         reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric),
                         Type::U64),
                 Operand::ValueOf(pointer),
                 Operand::ValueOf(operation),
                 Operand::ValueOf(location)});
        return value(result);
    }
};

class DirectSink : public direct::Interpreter {
public:
    DirectSink(runtime::arm64::GuestState& state, const DirectServices& services)
            : Interpreter({{reinterpret_cast<std::uint8_t*>(&state), sizeof(state)},
                           state.packed_flags,
                           state.pc,
                           state.numeric_control},
                          {services.opaque,
                           services.read,
                           services.write,
                           services.atomic,
                           nullptr,
                           services.guest_events})
            , state(state) {}

    template <runtime::ir::OpCode op> Result Emit(runtime::ir::Type type,
                                                  std::span<const Operand> args,
                                                  runtime::ir::OperationMetadata metadata = {}) {
        if (skipped) {
            Producer p;
            p.type = type;
            return {runtime::ir::Error::None, {p}};
        }
        return Interpreter::Emit<op>(type, args, metadata);
    }

    template <runtime::ir::OpCode op> Result Emit(runtime::ir::Type type,
                                                  std::initializer_list<Operand> args,
                                                  runtime::ir::OperationMetadata metadata = {}) {
        return Emit<op>(type, {args.begin(), args.size()}, metadata);
    }

    runtime::ir::Error SetMetadata(Producer& value, runtime::ir::OperationMetadata metadata) {
        return skipped ? runtime::ir::Error::None : Interpreter::SetMetadata(value, metadata);
    }

    Value LocalGoto(Value condition, runtime::ir::Type) {
        condition.number.extra = ++label;
        if (!skipped && condition.number.bits) skipped = label;
        return condition;
    }

    void LocalBind(Value value) {
        if (skipped == value.number.extra) skipped = 0;
    }

    Value Helper(runtime::arm64::Numeric family,
                 std::uint32_t word,
                 std::uint64_t pc,
                 std::uint32_t extra,
                 std::uint32_t) {
        Producer p;
        p.type = runtime::ir::Type::U64;
        if (skipped) return {p};
        Operand callback;
        callback.kind = runtime::ir::OperandKind::Lambda;
        const std::array<Operand, 5> args{
                callback,
                Operand::Immediate(
                        reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric)),
                Operand::Immediate(reinterpret_cast<std::uintptr_t>(&state)),
                Operand::Immediate(runtime::arm64::NumericCall(family, word, extra)),
                Operand::Immediate(pc)};
        const auto result = Interpreter::Emit<runtime::ir::OpCode::CallLambda>(
                runtime::ir::Type::U64, std::span<const Operand>{args});
        if (!result) throw direct::Exit{direct::ExitKind::Unsupported};
        p = result.GetValue().number;
        if (state.helper_status) throw direct::Exit{direct::ExitKind::Unsupported};
        observed_effects = true;
        return {p};
    }

private:
    runtime::arm64::GuestState& state;
    std::uint64_t label{}, skipped{};
};
}  // namespace swift::arm64::detail
