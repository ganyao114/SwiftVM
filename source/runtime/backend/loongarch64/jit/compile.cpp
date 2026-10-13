#include "compile.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include "runtime/backend/address_space.h"
#include "runtime/backend/function_plan.h"
#include "runtime/backend/loongarch64/jit/translator.h"
#include "runtime/common/variant_util.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"

namespace swift::runtime::backend::loongarch64 {
namespace {
using Definitions = std::unordered_set<ir::Inst*>;

void VerifyTerminal(const ir::Terminal& terminal, const Definitions& definitions) {
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            if (!definitions.contains(term.cond.Def())) throw std::runtime_error("LoongArch terminal has a foreign SSA input");
            VerifyTerminal(term.then_, definitions); VerifyTerminal(term.else_, definitions);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            VerifyTerminal(term.then_, definitions); VerifyTerminal(term.else_, definitions);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            if (!definitions.contains(term.value.Def())) throw std::runtime_error("LoongArch terminal has a foreign SSA input");
            for (const auto& item : term.cases) VerifyTerminal(item.then, definitions);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) VerifyTerminal(term.else_, definitions);
    });
}

void Prepare(const Module& module, ir::Block* block, ir::HIRFunction* function = nullptr,
             const Definitions* all_definitions = nullptr) {
    Definitions local;
    if (!all_definitions) for (auto& inst : block->GetInstList()) local.insert(&inst);
    const auto& definitions = all_definitions ? *all_definitions : local;
    for (auto& inst : block->GetInstList()) {
        if (!inst.IsPseudoOperation())
            for (auto* pseudo : inst.GetPseudoOperations())
                if (!definitions.contains(pseudo))
                    throw std::runtime_error("LoongArch unit has a foreign pseudo operation");
        const auto check = [&](ir::Value value) {
            if (!definitions.contains(value.Def())) throw std::runtime_error("LoongArch unit has a foreign SSA input");
        };
        for (u32 i = 0; i < ir::Inst::max_args; ++i) {
            auto& arg = inst.ArgAt(i);
            if (arg.IsValue()) check(arg.Get<ir::Value>());
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) check(arg.Get<ir::Lambda>().GetValue());
            else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>())
                if (param.data.IsValue()) check(param.data.value);
        }
    }
    VerifyTerminal(block->GetTerminal(), definitions);
    const auto& config = module.GetModuleConfig();
    if (config.HasOpt(Optimizations::ConstantFolding)) ir::ConstFoldingPass::Run(block, ResolveFeatureSet(config));
    if (config.HasOpt(Optimizations::DeadCodeRemove)) ir::DeadCodeEliminationPass::Run(block, function);
    if (!function) block->ReIdInstr();
}

void Publish(const std::shared_ptr<Module>& module, ir::Block* block, const CodeBuffer& buffer,
             u16 id, u32 recovery, ir::AddressNode* owner = nullptr) {
    module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                          block->GetStartLocation().Value(), buffer.exec_data, buffer.exec_data + recovery);
    if (!module->GetModuleConfig().read_only) {
        u64 decoded_size{};
        for (const auto& inst : block->GetInstList()) if (inst.GetOp() == ir::OpCode::AdvancePC)
            decoded_size += inst.GetArg<ir::Imm>(0).Get();
        if (decoded_size) block->SetEndLocation(block->GetStartLocation() + decoded_size);
        else if (block->GetEndLocation().Value() <= block->GetStartLocation().Value())
            block->SetEndLocation(block->GetStartLocation() + 1);
        module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner ? owner : block,
                block->GetStartLocation().Value(), block->GetEndLocation().Value());
        for (const auto& dependency : block->GetGuestCodeDependencies())
            module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner ? owner : block,
                    dependency.start.Value(), dependency.end.Value());
    }
    // Module lookups and the public dispatcher must never see executable code
    // before its IR owners, recovery metadata and SMC registrations are ready.
    auto& cached = block->GetJitCache();
    cached.cache_id = id; cached.offset_in = buffer.offset;
    cached.cache_size = buffer.size; cached.jit_state = JitState::Cached;
    module->GetAddressSpace().PushCodeCache(block->GetStartLocation(), buffer.exec_data);
}
}  // namespace

void* CompileBlock(const std::shared_ptr<Module>& module, ir::Block* block) {
    if (block->GetJitCache().jit_state == JitState::Cached) return module->GetJitCache(block->GetJitCache());
    Prepare(*module, block);
    JitContext context;
    context.ConfigureModule(module.get());
    JitTranslator translator{context}; translator.Translate(block);
    auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize());
    if (id == INVALID_CACHE_ID) return nullptr;
    try {
        context.Flush(buffer);
        module->RetainCodeIR(buffer.exec_data, block);
        if (translator.EmittedIR() != block) module->RetainCodeIR(buffer.exec_data, translator.EmittedIR());
        Publish(module, block, buffer, id, translator.RecoveryOffset());
    } catch (...) {
        block->GetJitCache() = JitCache{};
        module->ReclaimCode(buffer.exec_data);
        throw;
    }
    return buffer.exec_data;
}

void* CompileFunctions(const std::shared_ptr<Module>& module,
                       std::span<ir::HIRFunction* const> functions) {
    void* first{};
    for (auto* function : functions) {
        std::vector<ir::HIRBlock*> ordered;
        const auto start = function->GetFunction()->GetStartLocation();
        for (auto* hir : function->GetHIRBlocks()) if (hir && hir->GetBlock()->GetStartLocation() == start)
            ordered.push_back(hir);
        for (auto* hir : function->GetHIRBlocks())
            if (hir && hir->GetBlock()->GetStartLocation() != start &&
                hir->GetBlock()->GetStartLocation() != ir::Location::INVALID) ordered.push_back(hir);
        std::erase_if(ordered, [](auto* hir) { return !hir->GetBlock()->HasTerminal() && hir->GetInstList().empty(); });
        if (ordered.empty() || ordered.front()->GetBlock()->GetStartLocation() != start)
            throw std::runtime_error("LoongArch function has no decoded entry");
        Definitions definitions;
        for (auto* hir : ordered) for (auto& inst : hir->GetInstList()) definitions.insert(&inst);
        for (auto* hir : ordered) Prepare(*module, hir->GetBlock(), function, &definitions);
        auto plan = FunctionPlan::Build(function, ordered);
        JitContext context;
        context.ConfigureModule(module.get());
        context.ConfigureFlow(std::move(plan.targets), std::move(plan.stops), std::move(plan.locations),
                              std::move(plan.synthetic_labels));
        JitTranslator translator{context};
        translator.SetFunctionExits(std::move(plan.exits), std::move(plan.halt_checks));
        translator.Translate(plan.body.get());
        auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize());
        if (id == INVALID_CACHE_ID) return nullptr;
        auto* owner = function->GetFunction();
        IntrusivePtr<ir::Function> owner_guard;
        bool pushed{};
        // Keep module readers and SMC detachment behind this lock until the
        // publication (or its rollback) is complete. SMC drops its metadata
        // spinlock before waiting for this node lock.
        auto publication_lock = owner->LockWrite();
        try {
            context.Flush(buffer);
            module->RetainCodeIR(buffer.exec_data, plan.body.get());
            if (translator.EmittedIR() != plan.body.get()) module->RetainCodeIR(buffer.exec_data, translator.EmittedIR());
            module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, start.Value(),
                                  buffer.exec_data, buffer.exec_data + translator.RecoveryOffset());
            u64 end = start.Value() + 1;
            for (auto* hir : ordered) {
                auto* block = hir->GetBlock();
                u64 size{};
                for (const auto& inst : block->GetInstList()) if (inst.GetOp() == ir::OpCode::AdvancePC)
                    size += inst.GetArg<ir::Imm>(0).Get();
                block->SetEndLocation(block->GetStartLocation() + std::max(size, u64{1}));
                end = std::max(end, block->GetEndLocation().Value());
            }
            owner->SetEndLocation(ir::Location{end});
            // A rejected fresh HIR function still owns its raw Function. Do
            // not retain/register that owner before the module accepts it.
            if (!module->Push(owner)) throw std::runtime_error("failed to publish LoongArch function");
            function->ReleaseFunctionOwnership();
            owner_guard = owner;
            pushed = true;
            module->RetainCodeIR(buffer.exec_data, owner);
            if (!module->GetModuleConfig().read_only) for (auto* hir : ordered) {
                auto* block = hir->GetBlock();
                module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner,
                        block->GetStartLocation().Value(), block->GetEndLocation().Value());
                for (const auto& dependency : block->GetGuestCodeDependencies())
                    module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner,
                            dependency.start.Value(), dependency.end.Value());
            }
            auto& cached = owner->GetJitCache();
            cached.cache_id = id; cached.offset_in = buffer.offset;
            cached.cache_size = buffer.size; cached.jit_state = JitState::Cached;
            module->GetAddressSpace().PushCodeCache(start, buffer.exec_data);
            if (!first) first = buffer.exec_data;
        } catch (...) {
            if (pushed) {
                owner_guard->GetJitCache() = JitCache{};
                module->Remove(owner_guard.get());
            }
            module->ReclaimCode(buffer.exec_data);
            throw;
        }
    }
    return first;
}
}  // namespace swift::runtime::backend::loongarch64
