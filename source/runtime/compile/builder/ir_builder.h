#pragma once

#include "runtime/compile/analysis/guest_state.h"

#include "runtime/ir/hir_builder.h"
#include "runtime/compile/rules/value_rules.h"

#include <vector>

namespace swift::runtime::compile {
class IrBuilder {
public:
    explicit IrBuilder(ir::HIRBuilder* builder) : hir_builder(builder), ir_block() {}

    explicit IrBuilder(ir::Block* block) : ir_block(block), hir_builder() {}

    // Commits typed construction for one guest instruction, once its flag
    // outputs are known. Calls without a flags contract remain conservative.
    class GuestInstructionScope final {
    public:
        explicit GuestInstructionScope(IrBuilder& builder);
        ~GuestInstructionScope();
        GuestInstructionScope(const GuestInstructionScope&) = delete;
        GuestInstructionScope& operator=(const GuestInstructionScope&) = delete;
    private:
        IrBuilder& builder;
        int exceptions;
    };

    template <typename RetType = void, typename... Args>
        requires ir::meta::ConstructionArguments<Args...>
    ir::Inst* AppendInst(ir::OpCode op, Args&&... args) {
        const auto type = ir::meta::ResolveResultType<RetType>(op, args...);
        return AppendInst(op, type, std::forward<Args>(args)...);
    }

    template <typename... Args> requires ir::meta::ConstructionArguments<Args...>
    ir::Inst* AppendInst(ir::OpCode op, ir::ValueType type, Args&&... args) {
        return AppendInst(ir::meta::CheckArgumentPack<Args...>(op), type, std::forward<Args>(args)...);
    }

    template <typename... Args> requires ir::meta::ConstructionArguments<Args...>
    ir::Inst* AppendInst(ir::meta::OperationFor<Args...> operation,
                         ir::ValueType type, Args&&... args) {
        ir::Inst* inst = AppendRawTyped(operation, type, std::forward<Args>(args)...);
        AfterAppend(inst, FlagsIntent::Unknown(), type != ir::ValueType::VOID);
        return inst;
    }

    void ConfigureGuestState(const compile::GuestStateLayout* info, FeatureSet features,
                             BuilderCapabilities target = {});
    ir::Value LoadUniform(ir::Uniform uniform);
    ir::Void StoreUniform(ir::Uniform uniform, ir::Value value);

#define INST(name, ret, policy, ...)                                                                    \
    template <typename RetType = void, typename... Args>                                                \
        requires((!std::is_void_v<RetType> ||                                                           \
                  ir::meta::HasDefaultResultType<ir::meta::kResultPolicy_##name>) &&                    \
                 ir::meta::ConstructionArguments<Args...> &&                                            \
                 ir::meta::ValidArgumentPack<Args...>(ir::meta::kArgs_##name))                          \
    ir::ret name(Args&&... args) {                                                                      \
        const auto type = ir::meta::ResolveResultType<ir::meta::kResultPolicy_##name,                   \
                ir::meta::kResult_##name, RetType>(args...);                                            \
        ValidateTypedReturn<ir::ret>(type);                                                             \
        if constexpr (std::is_same_v<ir::ret, ir::Void>) {                                              \
            return ir::ret{AppendInst(ir::meta::KnownOperation<ir::OpCode::name, Args...>(),            \
                                     type, std::forward<Args>(args)...)};                               \
        } else {                                                                                        \
            return ir::ret{AppendTyped<ir::OpCode::name>(type, FlagsIntent::Unknown(), std::forward<Args>(args)...)}; \
        }                                                                                               \
    }                                                                                                   \
    template <typename... Args>                                                                         \
        requires (!std::is_same_v<ir::ret, ir::Void> &&                                                 \
                  ir::meta::ConstructionArguments<Args...> &&                                           \
                 ir::meta::ValidArgumentPack<Args...>(ir::meta::kArgs_##name))                          \
    ir::ret name(ir::ValueType type, FlagsIntent flags, Args&&... args) {                               \
        ValidateTypedReturn<ir::ret>(type);                                                             \
        return ir::ret{AppendTyped<ir::OpCode::name>(type, flags, std::forward<Args>(args)...)};        \
    }                                                                                                   \
    template <typename... Args>                                                                         \
        requires (!std::is_same_v<ir::ret, ir::Void> &&                                                 \
                  ir::meta::ConstructionArguments<Args...> &&                                           \
                 ir::meta::ValidArgumentPack<Args...>(ir::meta::kArgs_##name))                          \
    ir::ret name(ir::ValueType type, Args&&... args) {                                                  \
        return name(type, FlagsIntent::Unknown(), std::forward<Args>(args)...);                         \
    }
#include "runtime/ir/ir.inc"
#undef INST

    template <typename LambdaT, typename... Args>
        requires(ir::meta::ValidArgumentPack<ir::Lambda, Args...>(ir::meta::kArgs_CallLambda))
    ir::Value CallHost(ir::ValueType type, LambdaT l, Args&&... args) {
        return CallLambda(type, ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(FptrCast(l))}}, std::forward<Args>(args)...);
    }

    template <typename LambdaT, typename... Args>
        requires(ir::meta::ValidArgumentPack<ir::Lambda, Args...>(ir::meta::kArgs_CallLambda))
    ir::Value CallHostWithTraits(
            ir::ValueType type, ir::HelperCallTraits traits, LambdaT l, Args&&... args) {
        return CallLambda(
                type,
                ir::Lambda{ir::DataClass{ir::Imm{reinterpret_cast<VAddr>(FptrCast(l))}}, traits},
                std::forward<Args>(args)...);
    }

    template <typename LambdaT, typename... Args>
    ir::Value CallHostFPFree(ir::ValueType type, LambdaT l, Args&&... args) {
        return CallHostWithTraits(
                type,
                ir::HelperCallTraits{.host_fp = ir::HostFpEffect::FPCRTransparent},
                l,
                std::forward<Args>(args)...);
    }

    template <typename LambdaT, typename... Args> ir::Value CallHostWithUniformEffects(
            ir::ValueType type, ir::UniformEffectId effects, LambdaT l, Args&&... args) {
        return CallHostWithTraits(type, ir::HelperCallTraits{.uniform = effects}, l, std::forward<Args>(args)...);
    }

    template <typename LambdaT, typename... Args>
    ir::Value CallHostUniformPure(ir::ValueType type, LambdaT l, Args&&... args) {
        return CallHostWithUniformEffects(type, ir::UniformEffectId::None, l, std::forward<Args>(args)...);
    }

    template <typename LambdaT, typename... Args>
    ir::Value CallHostUniformPureFPFree(ir::ValueType type, LambdaT l, Args&&... args) {
        return CallHostWithTraits(type,
                                  ir::HelperCallTraits{
                                          .uniform = ir::UniformEffectId::None,
                                          .host_fp = ir::HostFpEffect::FPCRTransparent,
                                  },
                                  l,
                                  std::forward<Args>(args)...);
    }

    ir::HIRBuilder::ElseThen If(const ir::terminal::If& if_);

    ir::HIRBlock* LinkBlock(const ir::terminal::LinkBlock& switch_);

    void ExternalLinkBlock(const ir::terminal::ExternalLinkBlock& link);

    void RegisterExternalDirectLink(ir::Location target);

    void ReturnToDispatcher();

    void ReturnToHost();

    void Return();

    void RegisterCallReturn(ir::Location location);

    bool EndCommit() const;
    [[nodiscard]] bool IsFunctionMode() const { return hir_builder != nullptr; }
    [[nodiscard]] bool HasFunctionBlock(ir::Location location) const;

    void AddGuestCodeDependency(ir::Location start, ir::Location end) {
        if (hir_builder) {
            hir_builder->AddGuestCodeDependency(start, end);
        } else {
            ASSERT(ir_block);
            ir_block->AddGuestCodeDependency(start, end);
        }
    }

private:
    template <typename Edit>
    void RewriteInst(ir::Inst* inst, Edit&& edit) {
        if (hir_builder) hir_builder->RewriteInst(inst, std::forward<Edit>(edit));
        else edit();
    }

    template <typename Return>
    static void ValidateTypedReturn(ir::ValueType type) {
        if constexpr (requires { Return::TYPE; }) ASSERT(type == Return::TYPE);
    }

    // The schema or caller supplies the result before forwarding or folding.
    template <ir::OpCode op, typename... Args>
    ir::Value AppendTyped(ir::ValueType type, FlagsIntent flags, Args&&... args) {
        return AppendTypedNormalized<op>(type, flags,
                ir::meta::ForwardConstructionArgument(std::forward<Args>(args))...);
    }

    template <ir::OpCode op, typename... Args> requires ir::meta::ConstructionArguments<Args...>
    ir::Value AppendTypedNormalized(ir::ValueType type, FlagsIntent flags, Args&&... args) {
        if constexpr (op != ir::OpCode::CallLambda && op != ir::OpCode::CallLocation &&
                      op != ir::OpCode::CallDynamic) ASSERT(type != ir::ValueType::VOID);
        if constexpr (op == ir::OpCode::LoadUniform) {
            const auto value = LoadUniform(args...);
            ASSERT(value.Type() == type);
            return value;
        } else if constexpr (op == ir::OpCode::BitCast) {
            if (auto value = FoldSameTypeCast(type, ir::Value{args...}, flags)) {
                return *value;
            }
        }
        auto* inst = AppendRawTyped<op>(type, std::forward<Args>(args)...);
        AfterAppend(inst, flags, type != ir::ValueType::VOID);
        return ir::Value{inst};
    }

    template <ir::OpCode op, typename... Args> requires ir::meta::ConstructionArguments<Args...>
    ir::Inst* AppendRawTyped(ir::ValueType type, Args&&... args) {
        return AppendRawTyped(ir::meta::KnownOperation<op, Args...>(), type, std::forward<Args>(args)...);
    }

    template <typename... Args> requires ir::meta::ConstructionArguments<Args...>
    ir::Inst* AppendRawTyped(ir::meta::OperationFor<Args...> operation,
                            ir::ValueType type, Args&&... args) {
        return AppendRawConstructed(operation, type,
                ir::meta::ForwardConstructionArgument(std::forward<Args>(args))...);
    }

    template <typename... Args> requires ir::meta::ConstructionArguments<Args...>
    ir::Inst* AppendRawConstructed(ir::meta::OperationFor<Args...> operation,
                            ir::ValueType type, Args&&... args) {
        ir::Inst* inst{};
        if (hir_builder) {
            inst = hir_builder->AppendInst(operation, type, std::forward<Args>(args)...);
        } else if (ir_block) {
            inst = ir_block->AppendInst(operation, type, std::forward<Args>(args)...);
        } else {
            PANIC();
        }
        if ((target.add_sub_immediate || target.logical_immediate) && inst) {
            // Only real appends count, including metadata. HIR may coalesce
            // AdvancePC and return nullptr without advancing this position.
            const auto position = bound_index++;
            if (operation.Code() == ir::OpCode::LoadImm) {
                RecordImmediateSource(inst, position);
            }
        }
        return inst;
    }

    void UniformAfterAppend(ir::Inst* inst);
    void UniformOnLoad(ir::Inst* inst);
    void UniformOnStore(ir::Inst* inst);
    void UniformStoreFacts(u32 offset, ir::Value value, GuestStateForwardWrite write);
    void AfterAppend(ir::Inst* inst, FlagsIntent flags, bool typed);
    void FoldImmediate(ir::Inst* inst, FlagsIntent flags, u32 position);
    void FoldValue(ir::Inst* inst, FlagsIntent flags, u32 position);
    void RecordImmediateSource(const ir::Inst*, u32 position);
    void PruneImmediateSources();
    [[nodiscard]] bool HasImmediateSource(const ir::Inst*, u32 position) const;
    void BeginGuestInstruction();
    void CommitGuestInstruction();
    void CancelGuestInstruction();
    void ObserveFlagOutput(const ir::Inst& inst) const;
    void UniformInvalidateAll();
    [[nodiscard]] GuestStateRead UniformReadFor(ir::Uniform uniform,
                                               const GuestStateForwardAccess& access) const;

    ir::HIRBuilder* hir_builder;
    ir::Block* ir_block;
    bool end_decode{false};

    const compile::GuestStateLayout* guest_layout{};
    FeatureSet uniform_features{};
    BuilderCapabilities target{};
    bool uniform_builder{};
    // Decode-time guest-state forwarding, captured when this builder is configured.
    bool uniform_forward{};
    // Bound-backend decode: the unit is known to target the arm64
    // JIT, so the builder may also emit mapped uniform stores
    // (SetHost*) directly. The pass remains the general path.
    bool uniform_bound{};
    // Deferred queries retain this instruction's history through commit.
    // Outside a guest instruction, only the still-queryable suffix survives.
    struct ImmediateSource {
        const ir::Inst* inst;
        u32 position;
    };
    std::vector<ImmediateSource> immediate_sources;
    u32 bound_index{};
    struct PendingValue {
        ir::Inst* inst;
        FlagsIntent flags;
        u32 position;
    };
    StackVector<PendingValue, 8> pending_values;
    bool guest_instruction{};
    std::vector<GuestStateByteFact> uniform_bytes;
    GuestStateForwarding uniform_forwarding;
};

}  // namespace swift::runtime::compile
