#include "compile.h"
#include "function.h"
#include "native_call_arity.h"
#include "pair_pseudo_producer.h"
#include "runtime/backend/riscv64/link.h"
#include "runtime/compile/workspace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include "runtime/backend/address_space.h"
#include "runtime/backend/riscv64/jit/translator.h"
#include "runtime/common/variant_util.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/compile/passes/value_worklist.h"

namespace swift::runtime::backend::riscv64 {

namespace {
using Definitions = std::unordered_set<const ir::Inst*>;

bool ScalarFlagValue(ir::Value value) {
    return value.Type() != ir::ValueType::VOID && !ir::IsFloatValueType(value.Type()) &&
           ir::GetValueSizeByte(value.Type()) <= 8;
}

// An entry poll sees incoming flags; the terminal sees all final flags. No
// instruction between those boundaries may expose a partially executed unit.
bool PureScalarFlags(const Module& module, const ir::Block* block) {
    const auto terminal_safe = [&](const auto& recurse, const ir::Terminal& terminal) -> bool {
        return VisitVariant<bool>(terminal, [&](const auto& term) -> bool {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost> ||
                          std::is_same_v<T, ir::terminal::ReturnToDispatch> ||
                          std::is_same_v<T, ir::terminal::LinkBlock> ||
                          std::is_same_v<T, ir::terminal::LinkBlockFast>) return true;
            else if constexpr (std::is_same_v<T, ir::terminal::Condition>)
                return recurse(recurse, term.then_) && recurse(recurse, term.else_);
            else if constexpr (std::is_same_v<T, ir::terminal::If>)
                return ScalarFlagValue(term.cond) && recurse(recurse, term.then_) && recurse(recurse, term.else_);
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                if (!ScalarFlagValue(term.value)) return false;
                for (const auto& item : term.cases) if (!recurse(recurse, item.then)) return false;
                return true;
            } else return false;
        });
    };
    if (!terminal_safe(terminal_safe, block->GetTerminal())) return false;
    for (auto& inst : block->GetInstList()) {
        using O = ir::OpCode;
        switch (inst.GetOp()) {
            case O::Nop: case O::AdvancePC: case O::LoadImm: case O::Zero:
            case O::LoadUniform: case O::StoreUniform: case O::BitCast:
            case O::GetResult: case O::GetOperand:
            case O::Add: case O::Sub: case O::Mul: case O::MulHigh:
            case O::And: case O::Or: case O::Xor: case O::AndNot:
            case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
            case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
            case O::SignExtend: case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64:
            case O::TestZero: case O::TestNotZero: case O::TestBit:
            case O::GetFlags: case O::SaveFlags: case O::TestFlags: case O::TestNotFlags:
            case O::ClearFlags: case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
            case O::CondSet: case O::LocalCondSet: case O::CondSelect: break;
            default: return false;
        }
        if (inst.GetOp() == O::SaveFlags) {
            const auto value = inst.GetArg<ir::Value>(0);
            const auto producer = value.Def()->GetOp();
            // The portable pass treats its mask as definite writes. Native
            // RV flags deliberately preserve undefined AF/CV for producers
            // that do not calculate them; those masks cannot be admitted.
            auto definite = ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Parity;
            if (producer == O::Add || producer == O::Sub) definite = ir::Flags::All;
            else if (producer == O::And || producer == O::Or || producer == O::Xor ||
                     producer == O::AndNot || (producer == O::Mul && ir::GetValueSizeByte(value.Type()) < 8))
                definite |= ir::Flags::CV;
            if (True(inst.GetArg<ir::Flags>(1) & ~definite)) return false;
        }
        if (inst.ReturnType() != ir::ValueType::VOID &&
            (ir::IsFloatValueType(inst.ReturnType()) || ir::GetValueSizeByte(inst.ReturnType()) > 8))
            return false;
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst.ArgAt(slot);
            // ArgAt indexes physical slots: Operand's two values are already
            // visited separately, unlike GetArg's logical Operand view.
            if (arg.IsValue() && !ScalarFlagValue(arg.Get<ir::Value>())) return false;
            if (arg.GetType() == ir::ArgType::Flags && True(arg.Get<ir::Flags>() & ~ir::Flags::All))
                return false;
            if (arg.GetType() == ir::ArgType::Uniform) {
                const auto& uniform = arg.Get<ir::Uniform>();
                const auto type = uniform.GetType();
                if (type == ir::ValueType::VOID || ir::IsFloatValueType(type)) return false;
                if (ir::GetValueSizeByte(type) > 8) return false;
                // Native accesses follow the value/return width, not the
                // declared Uniform width. A narrow declaration cannot prove
                // a wider access near the buffer end non-faulting.
                const auto access_type = inst.GetOp() == O::StoreUniform
                        ? inst.GetArg<ir::Value>(1).Type() : inst.ReturnType();
                if (access_type == ir::ValueType::VOID || ir::IsFloatValueType(access_type)) return false;
                const auto width = ir::GetValueSizeByte(access_type);
                const auto capacity = module.GetAddressSpace().GetConfig().uniform_buffer_size;
                if (width > 8 || uniform.GetOffset() > capacity || width > capacity - uniform.GetOffset())
                    return false;
            }
        }
    }
    return true;
}

// Static emission accounting only. This is deliberately cold and opt-in:
// opcode bytes are not an execution profile or a PMU measurement.
bool EmissionProfileEnabled() {
    const auto& config = GetSvmConfig();
    return config.prof_is_set && std::atoi(config.prof.c_str()) >= 2;
}

void ReportEmission(bool enabled, const ir::Block* original,
                    const JitTranslator& translator, u32 host_bytes) {
    if (!enabled) return;
    const auto& stats = translator.Stats();
    u32 instructions{};
    std::array<u32, static_cast<size_t>(ir::OpCode::BASE_COUNT)> counts{};
    for (const auto& inst : translator.EmittedIR()->GetInstList()) {
        ++instructions;
        ++counts[static_cast<size_t>(inst.GetOp())];
    }
    std::fprintf(stderr,
                 "[rv64-emission] guest=%#llx host_bytes=%u ir_insts=%u direct=%u helpers=%u "
                 "frame_bytes=%u saved_gprs=%u saved_fprs=%u\n",
                 static_cast<unsigned long long>(original->GetStartLocation().Value()),
                 host_bytes, instructions, stats.direct, stats.helpers, stats.frame_size,
                 stats.saved_gprs, stats.saved_fprs);
    for (size_t opcode = 0; opcode < stats.bytes.size(); ++opcode) {
        if (!stats.bytes[opcode]) continue;
        std::fprintf(stderr, "[rv64-opbytes] guest=%#llx op=%s count=%u bytes=%u\n",
                     static_cast<unsigned long long>(original->GetStartLocation().Value()),
                     ir::GetIRMetaInfo(static_cast<ir::OpCode>(opcode)).name,
                     counts[opcode], stats.bytes[opcode]);
    }
}

void AppendDependencies(std::vector<SerialBlock>& blocks, const ir::Block* block) {
    for (const auto& dependency : block->GetGuestCodeDependencies()) {
        SerialBlock extent{dependency.start.Value(), dependency.end.Value(), UINT32_MAX};
        extent.entry_flags = SerialBlock::DependencyOnly;
        blocks.push_back(extent);
    }
}

void VerifyTerminal(const ir::Terminal& terminal, const Definitions& definitions) {
    const bool valid = ir::CheckTerminal(terminal, [&](ir::Value value) {
        if (!definitions.contains(value.Def()))
            throw std::runtime_error("RV64 terminal has a cross-block SSA input");
    });
    if (!valid) throw std::runtime_error("RV64 terminal has invalid arguments");
}

void Prepare(const Module& module,
             compile::CompileWorkspace& workspace,
             ir::Block* block,
             ir::HIRFunction* hir = nullptr,
             const Definitions* function_definitions = nullptr) {
    const auto& config = module.GetModuleConfig();
    const bool flags_enabled = config.HasOpt(Optimizations::FlagElimination);
    bool has_barrier{};
    Definitions local;
    if (!function_definitions) for (auto& inst : block->GetInstList()) local.insert(&inst);
    const auto& definitions = function_definitions ? *function_definitions : local;
    for (auto& inst : block->GetInstList()) {
        if (!inst.HasValidResultType())
            throw std::runtime_error("RV64 IR has an invalid result type");
        // ConstFolding rewrites arithmetic/argument references, never this
        // zero-argument observer. DCE runs after the flags admission gate.
        if (flags_enabled && inst.GetOp() == ir::OpCode::MemoryBarrierTSO) has_barrier = true;
        const auto check = [&](ir::Value value) {
            if (!definitions.contains(value.Def()))
                throw std::runtime_error("RV64 function block has a cross-block SSA input");
        };
        if (!inst.CheckArguments(check))
            throw std::runtime_error("RV64 IR has invalid arguments");
        VerifyNativeCallContracts(inst);
    }
    VerifyTerminal(block->GetTerminal(), definitions);
    for (const auto* anchor : block->GetLoopHoistMetadata().anchors)
        if (!ir::Value{anchor}.HasValidDataView() || !definitions.contains(anchor))
            throw std::runtime_error("RV64 hoist root has an invalid or unavailable data view");
    const auto features = ResolveFeatureSet(config);
    // Keep architectural uniforms and flag producers canonical. The ARM64
    // uniform/allocator pipeline installs host-register reads that helpers
    // cannot execute and uses a different pinned-register ABI.
    compile::RewriteContext rewrite{
            block, workspace, hir, config.HasOpt(Optimizations::DeadCodeRemove)};
    const compile::BuilderCapabilities target{};
    std::optional<compile::ValueWorklist> values;
    if (config.HasOpt(Optimizations::ConstantFolding)) {
        compile::BeginValueCleanup(
                values, rewrite, features, target, compile::ValueCleanup::Values);
    }
    // Admit scalar flags before DCE, preserving the original observer/barrier
    // gate. Later feedback cannot turn a rejected unit into an admitted one.
    const bool scalar_flags = flags_enabled && !has_barrier && PureScalarFlags(module, block);
    bool first_flags = true;
    for (;;) {
        if (scalar_flags && (first_flags || rewrite.HasFlagChanges())) {
            auto scalar_features = features;
            scalar_features.flags_branch_only = false;
            compile::EliminateFlags(rewrite, scalar_features, ir::Flags::All);
        }
        first_flags = false;
        if (config.HasOpt(Optimizations::DeadCodeRemove)) rewrite.CollectDead();
        if (!values || !values->Pending()) break;
        values->Drain();
    }
    // HIR's use table is indexed by function-wide IDs. Keep those IDs intact
    // until every block's DCE has finished; local renumbering would make a
    // later block resolve an input to another block's HIRValue.
    if (!hir) block->ReIdInstr();
}

bool HasCrossBlockValues(std::span<const ir::HIRBlock* const> blocks) {
    for (auto* block : blocks) {
        Definitions local;
        for (auto& inst : block->GetInstList()) local.insert(&inst);
        const auto foreign = [&](ir::Value value) { return !local.contains(value.Def()); };
        for (auto& inst : block->GetInstList()) for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst.ArgAt(slot);
            if (arg.IsValue() && foreign(arg.Get<ir::Value>())) return true;
            if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue() && foreign(arg.Get<ir::Lambda>().GetValue())) return true;
            if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>())
                if (param.data.IsValue() && foreign(param.data.value)) return true;
        }
        try { VerifyTerminal(block->GetBlock()->GetTerminal(), local); }
        catch (const std::runtime_error&) { return true; }
    }
    return false;
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
    (void)module->PublishLinkTarget(block->GetStartLocation(), entry, buffer.exec_data);
    if (!module->GetModuleConfig().read_only) {
        if (!owner) owner = block;
        module->GetAddressSpace().GetSmcTracker().RegisterNode(
                module, owner, block->GetStartLocation().Value(), block->GetEndLocation().Value());
        for (const auto& dependency : block->GetGuestCodeDependencies())
            module->GetAddressSpace().GetSmcTracker().RegisterNode(
                    module, owner, dependency.start.Value(), dependency.end.Value());
    }
}

}  // namespace

void* CompileBlock(const std::shared_ptr<Module>& module, ir::Block* block) {
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return nullptr;
    if (block->GetJitCache().jit_state == JitState::Cached)
        return module->GetJitCache(block->GetJitCache());
    const bool emission_profile = EmissionProfileEnabled();
    compile::CompileWorkspaceScope workspace;
    Prepare(*module, workspace.Get(), block);
    // Freeze the decoded extent before WidePlan clones the IR and instruction
    // resume captures its completion PC and identity. Publication must not
    // mutate that identity, including for read-only modules that skip SMC.
    u64 decoded_size{};
    for (const auto& inst : block->GetInstList())
        if (inst.GetOp() == ir::OpCode::AdvancePC)
            decoded_size += inst.GetArg<ir::Imm>(0).Get();
    if (decoded_size)
        block->SetEndLocation(block->GetStartLocation() + decoded_size);
    else if (block->GetEndLocation().Value() <= block->GetStartLocation().Value())
        block->SetEndLocation(block->GetStartLocation() + 1);
    JitContext context{true, HostFeatures::Detect(), module->GetAddressSpace().GetConfig().buffers_static_alloc};
    context.ConfigureModule(module.get());
    JitTranslator translator{context};
    translator.Translate(block);
    ReportEmission(emission_profile, block, translator, context.CurrentBufferSize());
    auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize());
    if (id == INVALID_CACHE_ID) return nullptr;
    context.Flush(buffer);
    if (!RegisterLinkSites(module, buffer, context.LinkSites())) {
        module->ReclaimCode(buffer.exec_data);
        throw std::runtime_error("failed to register RV64 block link sites");
    }
    module->RetainCodeIR(buffer.exec_data, block);
    if (translator.EmittedIR() != block) module->RetainCodeIR(buffer.exec_data, translator.EmittedIR());
    if (auto descriptor = translator.InstructionResume()) module->RegisterInstructionResume(buffer.exec_data, std::move(descriptor));
    Publish(module, block, buffer, id, 0, buffer.size, translator.RecoveryOffset());
    std::vector<SerialBlock> blocks{{block->GetStartLocation().Value(), block->GetEndLocation().Value(), 0}};
    AppendDependencies(blocks, block);
    if (!translator.InstructionResume())
        RecordCode(module, buffer, block->GetStartLocation().Value(), false, std::move(blocks), context.LinkSites());
    // Helpers retaining IR stay owned by the module; the serializer rejects
    // their process-local literals while ordinary native units remain cacheable.
    return buffer.exec_data;
}

void* CompileFunctions(const std::shared_ptr<Module>& module,
                       std::span<ir::HIRFunction* const> functions) {
    RetirementControl::Lease producer{module->GetRetirementControl(), RetirementControl::Kind::Producer};
    if (!producer) return nullptr;
    if (functions.empty()) return nullptr;
    for (auto* function : functions)
        if (!function || function->HasStandaloneBlocks()) return nullptr;
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
    compile::CompileWorkspaceScope workspace;
    const bool emission_profile = EmissionProfileEnabled();
    JitContext context{true, HostFeatures::Detect(), module->GetAddressSpace().GetConfig().buffers_static_alloc};
    context.ConfigureModule(module.get());
    std::vector<Entry> entries;
    std::vector<IntrusivePtr<ir::Block>> wide_owners;
    for (auto* function : functions) {
        // Verify/emit before publishing ownership so failed whole-function
        // compilation can fall back to a freshly decoded flat block.
        std::vector<const ir::HIRBlock*> ordered;
        const auto start = function->GetFunction()->GetStartLocation();
        for (auto* hir : function->GetHIRBlocks())
            if (hir && hir->GetBlock()->GetStartLocation() == start) ordered.push_back(hir);
        for (auto* hir : function->GetHIRBlocks())
            if (hir && hir->GetBlock()->GetStartLocation() != start &&
                hir->GetBlock()->GetStartLocation() != ir::Location::INVALID) ordered.push_back(hir);
        if (ordered.empty() || ordered.front()->GetBlock()->GetStartLocation() != start ||
            (!ordered.front()->GetBlock()->HasTerminal() && ordered.front()->GetBlock()->GetInstList().empty()))
            throw std::runtime_error("RV64 function has no decoded root block");
        std::erase_if(ordered, [](const ir::HIRBlock* hir) {
            return !hir->GetBlock()->HasTerminal() && hir->GetInstList().empty();
        });
        Definitions definitions;
        for (auto* hir : ordered) for (auto& inst : hir->GetInstList()) definitions.insert(&inst);
        for (auto* hir : ordered) {
            auto* block = function->EditBlock(hir).GetBlock();
            if (!block->HasTerminal() && block->GetInstList().empty()) continue;
            u64 decoded_size{};
            for (const auto& inst : block->GetInstList())
                if (inst.GetOp() == ir::OpCode::AdvancePC)
                    decoded_size += inst.GetArg<ir::Imm>(0).Get();
            block->SetEndLocation(block->GetStartLocation() + std::max<u64>(decoded_size, 1));
            Prepare(*module, workspace.Get(), block, function, &definitions);
        }
        u64 end = start.Value();
        for (auto* hir : ordered) end = std::max(end, hir->GetBlock()->GetEndLocation().Value());
        function->GetFunction()->SetEndLocation(ir::Location{end});
        if (HasCrossBlockValues(ordered)) {
            auto plan = FunctionPlan::Build(function, ordered);
            context.ConfigureFlow(std::move(plan.targets), std::move(plan.stops), std::move(plan.locations),
                                  std::move(plan.synthetic_labels));
            JitTranslator translator{context};
            translator.SetFunctionExits(std::move(plan.exits), std::move(plan.halt_checks));
            translator.Translate(plan.body.get());
            ReportEmission(emission_profile, plan.body.get(), translator, context.CurrentBufferSize());
            auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize());
            if (id == INVALID_CACHE_ID) return nullptr;
            context.Flush(buffer);
            if (!RegisterLinkSites(module, buffer, context.LinkSites())) {
                module->ReclaimCode(buffer.exec_data);
                throw std::runtime_error("failed to register RV64 function link sites");
            }
            if (!module->Push(function->GetFunction())) {
                module->ReclaimCode(buffer.exec_data);
                throw std::runtime_error("failed to publish RV64 function CFG");
            }
            function->MarkFunctionPublished();
            module->RetainCodeIR(buffer.exec_data, function->GetFunction());
            module->RetainCodeIR(buffer.exec_data, translator.EmittedIR());
            auto* root = function->EditBlock(ordered.front()).GetBlock();
            Publish(module, root, buffer, id, 0, buffer.size, translator.RecoveryOffset(), function->GetFunction());
            function->GetFunction()->GetJitCache() = root->GetJitCache();
            std::vector<SerialBlock> serial_blocks;
            for (auto* hir : ordered) {
                auto* b = hir->GetBlock();
                SerialBlock serial{b->GetStartLocation().Value(), b->GetEndLocation().Value(),
                                   b == root ? 0u : UINT32_MAX};
                if (b != root) serial.entry_flags = SerialBlock::MetadataOnly;
                serial_blocks.push_back(serial);
            }
            for (auto* hir : ordered) AppendDependencies(serial_blocks, hir->GetBlock());
            if (!module->GetModuleConfig().read_only) for (auto* hir : ordered) if (hir->GetBlock() != root) {
                auto* block = hir->GetBlock();
                module->GetAddressSpace().GetSmcTracker().RegisterNode(module, function->GetFunction(),
                        block->GetStartLocation().Value(), block->GetEndLocation().Value());
                for (const auto& dependency : block->GetGuestCodeDependencies())
                    module->GetAddressSpace().GetSmcTracker().RegisterNode(module, function->GetFunction(),
                            dependency.start.Value(), dependency.end.Value());
            }
            RecordCode(module, buffer, root->GetStartLocation().Value(), true, std::move(serial_blocks), context.LinkSites());
            return buffer.exec_data;
        }
        for (auto* hir : ordered) {
            if (!hir) continue;
            auto* block = function->EditBlock(hir).GetBlock();
            if (!block->HasTerminal() && block->GetInstList().empty()) continue;
            block->ReIdInstr();
            const auto offset = context.CurrentBufferSize();
            JitTranslator translator{context};
            translator.Translate(block);
            ReportEmission(emission_profile, block, translator, context.CurrentBufferSize() - offset);
            if (translator.EmittedIR() != block) wide_owners.emplace_back(translator.EmittedIR());
            entries.push_back({block, function->GetFunction(), offset, context.CurrentBufferSize() - offset,
                               translator.RecoveryOffset() - offset});
        }
    }
    if (entries.empty()) return nullptr;
    auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize());
    if (id == INVALID_CACHE_ID) return nullptr;
    context.Flush(buffer);
    if (!RegisterLinkSites(module, buffer, context.LinkSites())) {
        module->ReclaimCode(buffer.exec_data);
        throw std::runtime_error("failed to register RV64 function block link sites");
    }
    // Allocation ownership spans every independent ABI block. Its recovery
    // remains block-specific in the narrower ranges published below.
    module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                         functions.front()->GetFunction()->GetStartLocation().Value(), buffer.exec_data);
    for (const auto& owner : wide_owners) module->RetainCodeIR(buffer.exec_data, owner.get());
    for (auto* function : functions) {
        if (!module->Push(function->GetFunction())) {
            module->ReclaimCode(buffer.exec_data);
            throw std::runtime_error("failed to publish RV64 function");
        }
        function->MarkFunctionPublished();
        module->RetainCodeIR(buffer.exec_data, function->GetFunction());
    }
    for (const auto& entry : entries)
        Publish(module, entry.block, buffer, id, entry.offset, entry.size, entry.recovery, entry.owner);
    std::vector<SerialBlock> serial_blocks;
    for (const auto& entry : entries) serial_blocks.push_back({entry.block->GetStartLocation().Value(),
            entry.block->GetEndLocation().Value(), entry.offset});
    for (const auto& entry : entries) AppendDependencies(serial_blocks, entry.block);
    RecordCode(module, buffer, functions.front()->GetFunction()->GetStartLocation().Value(), true,
               std::move(serial_blocks), context.LinkSites());
    for (auto* function : functions) {
        auto& cached = function->GetFunction()->GetJitCache();
        cached = function->GetFunction()->FindBlock(function->GetFunction()->GetStartLocation())->GetJitCache();
        cached.cache_size = buffer.size;
    }
    return module->GetJitCache(functions.front()->GetFunction()->GetJitCache());
}

}  // namespace swift::runtime::backend::riscv64
