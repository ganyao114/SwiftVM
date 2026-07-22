#pragma once

#include <vector>
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
    // Value storage: one slot per IR inst, indexed by Inst::Id(). Each slot is
    // two u64 (16 bytes) so vectors up to V128 fit. Inst ids are dense because
    // Block::ReIdInstr() runs when the frontend sets the block terminal.
    //
    // NOTE: this deliberately does NOT use Inst::VirRegID() /
    // Block::GetVStackSize(): those are only assigned by the register
    // allocation pass, which never runs on the interpreter path (blocks
    // executed here come straight from the frontend decoder).
    static constexpr u32 kSlotStride = 2;

    using u128 = unsigned __int128;

    template <typename T>
    T GetReg(InterpStack& stack, ir::Value value) {
        T ret{};
        std::memcpy(&ret, &stack[size_t(value.Def()->Id()) * kSlotStride], sizeof(T));
        return ret;
    };

    template <typename T>
    void SetReg(InterpStack& stack, ir::Value value, T t) {
        std::memcpy(&stack[size_t(value.Def()->Id()) * kSlotStride], &t, sizeof(T));
    };

    u64 ReadScalar(InterpStack& stack, ir::Value value);
    u128 ReadVec(InterpStack& stack, ir::Value value);
    void WriteScalar(InterpStack& stack, ir::Inst* inst, u64 value);
    void WriteVec(InterpStack& stack, ir::Inst* inst, u128 value);

    u64 EvalDataClass(InterpStack& stack, const ir::DataClass& data);
    u64 EvalOperand(InterpStack& stack, const ir::Operand& operand);
    u64 EvalLambda(InterpStack& stack, ir::Lambda& lambda);
    u64 CallHostFunc(InterpStack& stack,
                     ir::Lambda& lambda,
                     const std::vector<ir::DataClass>& args);

    // Guest flags (state.host_cpu_flags) helpers; the layout mirrors the JIT
    // flags register (x26), see interpreter.cpp.
    void SaveGuestFlags(InterpStack& stack, ir::Inst* def, ir::Flags flags);
    bool TestGuestFlags(ir::Flags flags);
    void ClearGuestFlags(ir::Flags flags);
    bool EvalCondition(ir::Cond cond);

    HaltReason RunTerminal(const ir::Terminal& terminal, InterpStack& stack);

    State& state;
    ir::Block* block;
};

}  // namespace swift::runtime::backend::interp
