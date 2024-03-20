#pragma once

#include "base/common_funcs.h"
#include "runtime/backend/context.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend::interp {

using InterpStack = std::span<u64>;

class Interpreter {
public:
    explicit Interpreter(State& state, ir::Block* block);

    HaltReason Run();

    HaltReason Run(ir::Inst* inst, InterpStack &stack);

#define INST(name, ...) void Run##name(ir::Inst* inst, InterpStack &stack);
#include "runtime/ir/ir.inc"
#undef INST

private:

    template <typename T>
    T GetReg(ir::Value value, InterpStack& stack) {
        auto buf = &stack[value.Def()->VirRegID()];
        T ret;
        std::memcpy(&ret, buf, sizeof(T));
        return std::move(ret);
    };

    template <typename T>
    T GetVReg(ir::Value value, InterpStack& stack) {
        auto buf = &stack[stack.size() - 2 * value.Def()->VirRegID()];
        T ret;
        std::memcpy(&ret, buf, sizeof(T));
        return std::move(ret);
    };

    State& state;
    ir::Block* block;
};

}  // namespace swift::runtime::backend::interp