#pragma once

#include <utility>

#include "check.h"
#include "runtime/ir/builder.h"

namespace swift::tests::ir_builder {
inline void Need(runtime::ir::Error error) {
    Check(error == runtime::ir::Error::None, "IR lifecycle");
}

inline runtime::ir::Value Need(runtime::ir::AppendResult result) {
    Check(bool(result), "IR append");
    return result.GetValue();
}

inline runtime::ir::BlockRef Block(runtime::ir::Builder& builder) {
    const auto result = builder.CreateBlock();
    Check(bool(result), "IR block creation");
    return result.block;
}

inline runtime::ir::Program Submit(runtime::ir::Builder& builder) {
    auto result = builder.Submit();
    Check(bool(result), "IR submit");
    return std::move(result.program);
}

inline runtime::ir::Value Constant(runtime::ir::Builder& builder, std::uint64_t value) {
    return Need(builder.Emit<runtime::ir::OpCode::LoadImm>(
            runtime::ir::Type::U64,
            {runtime::ir::Operand::Immediate(value, runtime::ir::Type::U64)}));
}
}  // namespace swift::tests::ir_builder
