//
// Created by 甘尧 on 2023/12/19.
//

#pragma once

#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"

#include <unordered_map>
#include <vector>

namespace swift::runtime::ir {

class Assembler {
public:
    explicit Assembler(HIRBuilder* builder) : hir_builder(builder), ir_block() {}

    explicit Assembler(Block* block) : ir_block(block), hir_builder() {}

    // Flushes pending uniform restamps: a block can end without a
    // terminal forward through this Assembler (decode limit /
    // fallthrough handled by the caller).
    ~Assembler();

    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>
    Inst* AppendInst(OpCode op, const Args&... args) {
        Inst* inst = AppendRaw<RetType>(op, std::forward<const Args&>(args)...);
        if (uniform_builder && inst) {
            UniformAfterAppend(inst);
        }
        if (uniform_bound && inst) {
            BoundFoldImmediate(inst);
            BoundFoldExtend(inst);
        }
        if (uniform_forward) {
            UniformFlushRestamps();
        }
        return inst;
    }

    // Smart-builder uniform shadow (step 4 SB0): mirrors
    // UniformEliminationPass's forward byte-fact table at append time
    // and counts the loads it would fold or convert, without changing
    // any IR. Enabled only under SVM_FOLDCENSUS until SB1.
    void EnableUniformBuilder(const UniformInfo* info, FeatureSet features,
                              bool bound_backend = false);
    Value LoadUniform(Uniform uniform);
    Void StoreUniform(Uniform uniform, Value value);

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

    void ExternalLinkBlock(const terminal::ExternalLinkBlock& link);

    void RegisterExternalDirectLink(Location target);

    void ReturnToDispatcher();

    void ReturnToHost();

    void Return();

    void RegisterCallReturn(Location location);

    bool EndCommit() const;
    [[nodiscard]] bool IsFunctionMode() const { return hir_builder != nullptr; }
    [[nodiscard]] bool HasFunctionBlock(Location location) const;

    void AddGuestCodeDependency(Location start, Location end) {
        if (hir_builder) {
            hir_builder->AddGuestCodeDependency(start, end);
        } else {
            ASSERT(ir_block);
            ir_block->AddGuestCodeDependency(start, end);
        }
    }

private:
    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>
    Inst* AppendRaw(OpCode op, const Args&... args) {
        Inst* inst{};
        if (hir_builder) {
            inst = hir_builder->AppendInst<RetType>(op, std::forward<const Args&>(args)...);
        } else if (ir_block) {
            inst = ir_block->AppendInst<RetType>(op, std::forward<const Args&>(args)...);
        } else {
            PANIC();
        }
        if (uniform_bound && inst) {
            // Only LoadImm positions are ever looked up (immediate
            // fold windows); counting every append keeps distances
            // exact without a hash insert per instruction.
            if (op == OpCode::LoadImm) {
                bound_positions.emplace(inst, bound_index);
            }
            ++bound_index;
        }
        return inst;
    }

    struct UniformByteFact {
        Value value{};
        u8 offset{};
        [[nodiscard]] bool Defined() const { return value.Defined(); }
    };
    struct UniformFold {
        bool ok{};
        bool extract{};  // BitExtract instead of BitCast alias
        Value value{};
        u8 offset{};
    };
    void UniformAfterAppend(Inst* inst);
    // The pass re-stamps folded/mapped loads with the Uniform
    // descriptor type, overriding any SetType the decoder applied
    // to the returned Value in between. Mirror that at the next
    // append so downstream sees the identical return type.
    void UniformFlushRestamps();
    void UniformOnLoad(Inst* inst);
    void UniformOnStore(Inst* inst);
    void UniformStoreFacts(Uniform uniform, Value value);
    void BoundFoldImmediate(Inst* inst);
    void BoundFoldExtend(Inst* inst);
    void UniformInvalidateAll();
    void UniformInvalidateHelper(const Inst& inst);
    [[nodiscard]] UniformFold UniformFoldFor(Uniform uniform) const;
    [[nodiscard]] bool UniformWouldFold(Uniform uniform) const {
        return UniformFoldFor(uniform).ok;
    }

    HIRBuilder* hir_builder;
    ir::Block* ir_block;
    bool end_decode{false};

    const UniformInfo* uniform_info{};
    FeatureSet uniform_features{};
    bool uniform_builder{};
    // SB1: actually emit the folded/converted node instead of the
    // LoadUniform the pass would have rewritten. Env-gated
    // (SVM_BUILDER_UNIFORM) until dump-equivalence gates pass.
    bool uniform_forward{};
    // Bound-backend decode: the unit is known to target the arm64
    // JIT, so the builder may also emit mapped uniform stores
    // (SetHost*) directly. The pass remains the general path.
    bool uniform_bound{};
    // Append positions for bound-mode use windows (mirrors the
    // const-folding pass's per-block instruction index).
    std::unordered_map<Inst*, u32> bound_positions;
    u32 bound_index{};
    std::vector<UniformByteFact> uniform_bytes;
    std::unordered_map<Inst*, std::vector<UniformByteFact>> uniform_labels;
    std::vector<std::pair<Inst*, ValueType>> uniform_restamps;
};

}  // namespace swift::runtime::ir
