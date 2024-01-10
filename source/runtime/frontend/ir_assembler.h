//
// Created by 甘尧 on 2023/12/19.
//

#pragma once

#include <span>
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class Assembler {
public:

    explicit Assembler(HIRBuilder* builder) : hir_builder(builder) {}

#define INST(name, ret, ...)                                                                       \
    template <typename... Args> ret name(const Args&... args) {                                    \
        return ret{hir_builder->AppendInst(OpCode::name, std::forward<const Args&>(args)...)};     \
    }
#include "runtime/ir/ir.inc"
#undef INST

    template<typename Lambda, typename... Args>
    Value CallHost(Lambda l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(FptrCast(l), std::forward<const Args&>(args)...);
    }

    HIRBuilder::ElseThen If(const terminal::If& if_);

    HIRBlock* LinkBlock(const terminal::LinkBlock& switch_);

    void ReturnToDispatcher();

    void ReturnToHost();

    void Return();

private:
    HIRBuilder *hir_builder{};
};

}
