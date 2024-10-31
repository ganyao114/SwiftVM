//
// Created by 甘尧 on 2023/12/19.
//

#pragma once

#include <span>
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class Assembler {
public:
    explicit Assembler(HIRBuilder* builder) : hir_builder(builder), ir_block() {}

    explicit Assembler(Block* block) : ir_block(block), hir_builder() {}

    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>
    Inst* AppendInst(OpCode op, const Args&... args) {
        if (hir_builder) {
            return hir_builder->AppendInst<RetType>(op, std::forward<const Args&>(args)...);
        } else if (ir_block) {
            return ir_block->AppendInst<RetType>(op, std::forward<const Args&>(args)...);
        } else {
            PANIC();
        }
    }

#define INST(name, ret, ...)                                                                       \
    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>                    \
    ret name(const Args&... args) {                                                                \
        return ret{AppendInst(OpCode::name, std::forward<const Args&>(args)...)};                  \
    }
#include "runtime/ir/ir.inc"
#undef INST

    template <typename Lambda, typename... Args> Value CallHost(Lambda l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(Lambda{Imm{reinterpret_cast<VAddr>(FptrCast(l))}},
                          std::forward<const Args&>(args)...);
    }

    HIRBuilder::ElseThen If(const terminal::If& if_);

    HIRBlock* LinkBlock(const terminal::LinkBlock& switch_);

    void ReturnToDispatcher();

    void ReturnToHost();

    void Return();

    bool EndCommit() const;

private:
    HIRBuilder* hir_builder;
    ir::Block* ir_block;
    bool end_decode{false};
};

}  // namespace swift::runtime::ir
