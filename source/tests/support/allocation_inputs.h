#pragma once
#include "case_support.h"
namespace {
static swift::runtime::ir::Block* BuildScratchPressureBlock(unsigned live) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x1000});
    std::vector<Value> scalars;
    scalars.reserve(live);
    for (unsigned i = 0; i < live; i++) {
        scalars.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // Vector values come from uniforms, the shape the x86 frontend produces
    // for XMM registers (a V128 LoadImm is not a form the backend emits).
    auto lhs = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{16, ValueType::V128});
    auto rhs = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{32, ValueType::V128});
    auto sum = block->VecFAdd<TypedValue<ValueType::V128>>(lhs, rhs, Imm{32u});
    block->StoreUniform(Uniform{48, ValueType::V128}, sum);
    // Consume the scalars *after* the float op, in reverse definition order.
    Value acc = scalars.back();
    for (int i = static_cast<int>(live) - 2; i >= 0; i--) {
        acc = block->Add(acc, Operand{scalars[i]});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, acc);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Companion to the block above, aimed at the *reload* half of the budget:
// every value is read twice, far apart, so the linear scan spills most of them
// and the second read of each becomes a reload into a scratch register on top
// of the reading opcode's own budget. The final Add names one spilled value
// twice, which costs one register only because JitContext memoizes reloads per
// (instruction, value).
static swift::runtime::ir::Block* BuildReloadPressureBlock(unsigned live) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x2000});
    std::vector<Value> values;
    values.reserve(live);
    for (unsigned i = 0; i < live; i++) {
        values.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // First read in reverse order: nothing dies until the very end, so the
    // register file is saturated for the whole block.
    Value first = values.back();
    for (int i = static_cast<int>(live) - 2; i >= 0; i--) {
        first = block->Add(first, Operand{values[i]});
    }
    // Second read of every value, by which point most of them are spilled.
    Value second = values.front();
    for (unsigned i = 1; i < live; i++) {
        second = block->Add(second, Operand{values[i]});
    }
    auto both = block->Add(first, Operand{second});
    // One instruction naming the same value twice, and deliberately the value
    // defined first: it is live across the whole block, so the scan spills it,
    // and both reads become reloads of the same slot. JitContext must serve
    // them from one scratch register -- that memoisation is what the reload
    // accounting above is entitled to assume.
    // And, not Add: its emitter is one of the few that actually spends its
    // whole three-register budget, leaving no slack to absorb a second reload.
    auto doubled = block->And(values.front(), Operand{values.front()});
    auto total = block->Add(both, Operand{doubled});
    block->StoreUniform(Uniform{0, ValueType::U32}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}
}
