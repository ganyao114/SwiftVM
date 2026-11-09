#pragma once

#include "build_ir.h"
#include "interpreter.h"
#include "runtime/ir/direct/interpreter.h"
#include "semantics.h"

namespace swift::arm32::detail {
namespace direct = runtime::ir::direct;

class IRSink : public runtime::ir::Builder {
public:
    static constexpr bool IsDirect = false;
    using Value = runtime::ir::Value;
    using Operand = runtime::ir::Operand;
    using Producer = runtime::ir::NodeId;

    IRSink(runtime::ir::Scope scope, runtime::ir::TargetInfo target, runtime::ir::Program reuse)
            : Builder(scope, target, std::move(reuse)) {}

    Producer ProducerOf(Value value) const { return value.node; }

    runtime::ir::OperationMetadata Metadata(Producer node) const {
        return Operations()[node.value].Metadata();
    }

    Value LocalGoto(Value condition, runtime::ir::Type view) {
        const auto result =
                Emit<runtime::ir::OpCode::Goto>(view, {Operand::ValueOf(condition, view)});
        if (!result) throw Rejected{BuildError::IRRejected, result.error};
        return result.GetValue();
    }

    void LocalBind(Value label) {
        const auto result = Emit<runtime::ir::OpCode::BindLabel>(runtime::ir::Type::Void,
                                                                 {Operand::ValueOf(label)});
        if (!result) throw Rejected{BuildError::IRRejected, result.error};
    }
};

class DirectSink : public direct::Interpreter {
public:
    static constexpr bool IsDirect = true;

    DirectSink(runtime::arm32::GuestState& state, const DirectServices& services)
            : Interpreter({{reinterpret_cast<u8*>(&state), sizeof(state)},
                           state.packed_flags,
                           state.pc,
                           state.numeric_control},
                          {services.opaque,
                           services.read,
                           services.write,
                           services.atomic,
                           nullptr,
                           services.guest_events}) {}

    bool Truthy(Value value) const noexcept { return value.number.bits != 0; }

    template <runtime::ir::OpCode op> Result Emit(runtime::ir::Type type,
                                                  std::span<const Operand> args,
                                                  runtime::ir::OperationMetadata metadata = {}) {
        if (skipped) {
            Producer value;
            value.type = type;
            return {runtime::ir::Error::None, {value}};
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
        if (!skipped && Truthy(condition)) skipped = label;
        return condition;
    }

    void LocalBind(Value value) {
        if (skipped == value.number.extra) skipped = 0;
    }

private:
    std::uint64_t label{}, skipped{};
};
}  // namespace swift::arm32::detail
