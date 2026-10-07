#include "compile.h"

#include <set>
#include <stdexcept>
#include <vector>
#include "runtime/backend/address_space.h"
#include "runtime/backend/riscv64/jit/translator.h"
#include "runtime/common/variant_util.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"

namespace swift::runtime::backend::riscv64 {

namespace {

void VerifyTerminal(const ir::Terminal& terminal, const std::set<ir::Inst*>& definitions) {
    const auto check = [&](ir::Value value) {
        if (!definitions.contains(value.Def()))
            throw std::runtime_error("RV64 terminal has a cross-block SSA input");
    };
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            check(term.cond);
            VerifyTerminal(term.then_, definitions);
            VerifyTerminal(term.else_, definitions);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            VerifyTerminal(term.then_, definitions);
            VerifyTerminal(term.else_, definitions);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            check(term.value);
            for (const auto& item : term.cases) VerifyTerminal(item.then, definitions);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            VerifyTerminal(term.else_, definitions);
        }
    });
}

void Prepare(const Module& module, ir::Block* block, ir::HIRFunction* hir = nullptr) {
    std::set<ir::Inst*> definitions;
    for (auto& inst : block->GetInstList()) definitions.insert(&inst);
    for (auto& inst : block->GetInstList()) {
        const auto check = [&](ir::Value value) {
            if (!definitions.contains(value.Def()))
                throw std::runtime_error("RV64 function block has a cross-block SSA input");
        };
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst.ArgAt(slot);
            if (arg.IsValue()) check(arg.Get<ir::Value>());
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) check(arg.Get<ir::Lambda>().GetValue());
            else if (arg.IsParams())
                for (auto& param : arg.Get<ir::Params>()) if (param.data.IsValue()) check(param.data.value);
        }
    }
    VerifyTerminal(block->GetTerminal(), definitions);
    const auto& config = module.GetModuleConfig();
    const auto features = ResolveFeatureSet(config);
    // Keep architectural uniforms and flag producers canonical. The ARM64
    // uniform/allocator pipeline installs host-register reads that helpers
    // cannot execute and uses a different pinned-register ABI.
    if (config.HasOpt(Optimizations::ConstantFolding))
        ir::ConstFoldingPass::Run(block, features);
    if (config.HasOpt(Optimizations::DeadCodeRemove))
        ir::DeadCodeEliminationPass::Run(block, hir);
    // HIR's use table is indexed by function-wide IDs. Keep those IDs intact
    // until every block's DCE has finished; local renumbering would make a
    // later block resolve an input to another block's HIRValue.
    if (!hir) block->ReIdInstr();
}

void Publish(const std::shared_ptr<Module>& module, ir::Block* block,
             const CodeBuffer& buffer, u16 id, u32 offset, u32 size, u32 recovery,
             ir::AddressNode* owner = nullptr) {
    auto& cached = block->GetJitCache();
    cached.cache_id = id;
    cached.offset_in = buffer.offset + offset;
    cached.cache_size = size;
    cached.jit_state = JitState::Cached;
    auto* entry = buffer.exec_data + offset;
    module->AddFaultEntry(entry, entry + size, block->GetStartLocation().Value(),
                          buffer.exec_data, entry + recovery);
    module->GetAddressSpace().PushCodeCache(block->GetStartLocation(), entry);
    if (!module->GetModuleConfig().read_only) {
        if (!owner) owner = block;
        u64 decoded_size{};
        for (auto& inst : block->GetInstList()) {
            if (inst.GetOp() == ir::OpCode::AdvancePC)
                decoded_size += inst.GetArg<ir::Imm>(0).Get();
        }
        if (decoded_size)
            block->SetEndLocation(block->GetStartLocation() + decoded_size);
        else if (block->GetEndLocation().Value() <= block->GetStartLocation().Value())
            block->SetEndLocation(block->GetStartLocation() + 1);
        module->GetAddressSpace().GetSmcTracker().RegisterNode(
                module, owner, block->GetStartLocation().Value(), block->GetEndLocation().Value());
        for (const auto& dependency : block->GetGuestCodeDependencies())
            module->GetAddressSpace().GetSmcTracker().RegisterNode(
                    module, owner, dependency.start.Value(), dependency.end.Value());
    }
}

}  // namespace

void* CompileBlock(const std::shared_ptr<Module>& module, ir::Block* block) {
    if (block->GetJitCache().jit_state == JitState::Cached)
        return module->GetJitCache(block->GetJitCache());
    Prepare(*module, block);
    JitContext context{true, HostFeatures::Detect(), module->GetAddressSpace().GetConfig().buffers_static_alloc};
    JitTranslator translator{context};
    translator.Translate(block);
    auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize());
    if (id == INVALID_CACHE_ID) return nullptr;
    context.Flush(buffer);
    module->RetainCodeIR(buffer.exec_data, block);
    Publish(module, block, buffer, id, 0, buffer.size, translator.RecoveryOffset());
    // The module/SMC node owns the IR until its code is retired. Helpers embed
    // process-local IR and function pointers, so this code is not serialized.
    return buffer.exec_data;
}

void* CompileFunctions(const std::shared_ptr<Module>& module,
                       std::span<ir::HIRFunction* const> functions) {
    if (functions.size() > 1) {
        void* first{};
        for (auto* function : functions) {
            auto* entry = CompileFunctions(module, std::span{&function, 1});
            if (!entry) return nullptr;
            if (!first) first = entry;
        }
        return first;
    }
    struct Entry { ir::Block* block; ir::Function* owner; u32 offset; u32 size; u32 recovery; };
    JitContext context{true, HostFeatures::Detect(), module->GetAddressSpace().GetConfig().buffers_static_alloc};
    std::vector<Entry> entries;
    for (auto* function : functions) {
        // Verify/emit before publishing ownership so failed whole-function
        // compilation can fall back to a freshly decoded flat block.
        std::vector<ir::HIRBlock*> ordered;
        const auto start = function->GetFunction()->GetStartLocation();
        for (auto* hir : function->GetHIRBlocks())
            if (hir && hir->GetBlock()->GetStartLocation() == start) ordered.push_back(hir);
        for (auto* hir : function->GetHIRBlocks())
            if (hir && hir->GetBlock()->GetStartLocation() != start &&
                hir->GetBlock()->GetStartLocation() != ir::Location::INVALID) ordered.push_back(hir);
        if (ordered.empty() || ordered.front()->GetBlock()->GetStartLocation() != start ||
            (!ordered.front()->GetBlock()->HasTerminal() && ordered.front()->GetBlock()->GetInstList().empty()))
            throw std::runtime_error("RV64 function has no decoded root block");
        for (auto* hir : ordered) {
            auto* block = hir->GetBlock();
            if (!block->HasTerminal() && block->GetInstList().empty()) continue;
            u64 decoded_size{};
            for (const auto& inst : block->GetInstList())
                if (inst.GetOp() == ir::OpCode::AdvancePC)
                    decoded_size += inst.GetArg<ir::Imm>(0).Get();
            block->SetEndLocation(block->GetStartLocation() + std::max<u64>(decoded_size, 1));
            Prepare(*module, block, function);
        }
        u64 end = start.Value();
        for (auto* hir : ordered) end = std::max(end, hir->GetBlock()->GetEndLocation().Value());
        function->GetFunction()->SetEndLocation(ir::Location{end});
        for (auto* hir : ordered) {
            if (!hir) continue;
            auto* block = hir->GetBlock();
            if (!block->HasTerminal() && block->GetInstList().empty()) continue;
            block->ReIdInstr();
            const auto offset = context.CurrentBufferSize();
            JitTranslator translator{context};
            translator.Translate(block);
            entries.push_back({block, function->GetFunction(), offset, context.CurrentBufferSize() - offset,
                               translator.RecoveryOffset() - offset});
        }
    }
    if (entries.empty()) return nullptr;
    auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize());
    if (id == INVALID_CACHE_ID) return nullptr;
    context.Flush(buffer);
    for (auto* function : functions) {
        if (!module->Push(function->GetFunction())) {
            module->ReclaimCode(buffer.exec_data);
            throw std::runtime_error("failed to publish RV64 function");
        }
        function->ReleaseFunctionOwnership();
        module->RetainCodeIR(buffer.exec_data, function->GetFunction());
    }
    for (const auto& entry : entries)
        Publish(module, entry.block, buffer, id, entry.offset, entry.size, entry.recovery, entry.owner);
    for (auto* function : functions) {
        auto& cached = function->GetFunction()->GetJitCache();
        cached = function->GetFunction()->FindBlock(function->GetFunction()->GetStartLocation())->GetJitCache();
        cached.cache_size = buffer.size;
    }
    return module->GetJitCache(functions.front()->GetFunction()->GetJitCache());
}

}  // namespace swift::runtime::backend::riscv64
