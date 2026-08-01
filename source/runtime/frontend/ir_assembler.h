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
        static_assert(meta::ValidArgCount(OpCode::name, meta::kArgs_##name.size(),                 \
                                          sizeof...(Args)),                                        \
                    #name ": argument count mismatch (see ir.inc)");                               \
        return ret{AppendInst<RetType>(OpCode::name, std::forward<const Args&>(args)...)};         \
    }
#include "runtime/ir/ir.inc"
#undef INST

    template <typename LambdaT, typename... Args> Value CallHost(LambdaT l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(Lambda{Imm{reinterpret_cast<VAddr>(FptrCast(l))}},
                          std::forward<const Args&>(args)...);
    }

    template <typename LambdaT, typename... Args>
    Value CallHostWithTraits(HelperCallTraits traits, LambdaT l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(
                Lambda{DataClass{Imm{reinterpret_cast<VAddr>(FptrCast(l))}}, traits},
                std::forward<const Args&>(args)...);
    }

    template <typename LambdaT, typename... Args>
    Value CallHostFPFree(LambdaT l, const Args&... args) {
        return CallHostWithTraits(
                HelperCallTraits{.host_fp = HostFpEffect::FPCRTransparent},
                l,
                std::forward<const Args&>(args)...);
    }

    template <typename LambdaT, typename... Args>
    Value CallHostWithUniformEffects(UniformEffectId effects, LambdaT l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(
                Lambda{DataClass{Imm{reinterpret_cast<VAddr>(FptrCast(l))}}, effects},
                std::forward<const Args&>(args)...);
    }

    template <typename LambdaT, typename... Args>
    Value CallHostUniformPure(LambdaT l, const Args&... args) {
        return CallHostWithUniformEffects(UniformEffectId::None, l,
                                          std::forward<const Args&>(args)...);
    }

    template <typename LambdaT, typename... Args>
    Value CallHostUniformPureFPFree(LambdaT l, const Args&... args) {
        return CallHostWithTraits(
                HelperCallTraits{
                        .uniform = UniformEffectId::None,
                        .host_fp = HostFpEffect::FPCRTransparent,
                },
                l,
                std::forward<const Args&>(args)...);
    }

    HIRBuilder::ElseThen If(const terminal::If& if_);

    HIRBlock* LinkBlock(const terminal::LinkBlock& switch_);

    void ReturnToDispatcher();

    void ReturnToHost();

    void Return();

    bool EndCommit() const;
    [[nodiscard]] bool IsFunctionMode() const { return hir_builder != nullptr; }

private:
    HIRBuilder* hir_builder;
    ir::Block* ir_block;
    bool end_decode{false};
};

}  // namespace swift::runtime::ir
