#include "compiler.h"
#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/compile/workspace.h"

#include "audit.h"
#include "certificates.h"
#include "fixed_validation.h"
#include "internal.h"
#include "location_preparation.h"
#include "lower_atomics.h"
#include "lower_edges.h"

#include <algorithm>
#include <array>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/allocation_preparation.h"
#include "runtime/common/perf_stats.h"
#include "runtime/common/signal_diagnostic.h"
#include "runtime/common/svm_config.h"
#include "runtime/common/variant_util.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/allocate/controller.h"
#include "runtime/compile/passes/loop_hoist.h"
#include "runtime/compile/pipeline.h"
#include "runtime/compile/unit.h"
#include "runtime/ir/hir_builder.h"
#include "target.h"

namespace swift::runtime::backend::arm64 {

namespace {
bool IsArm64(const std::shared_ptr<Module>& module) {
    return module && module->GetAddressSpace().GetConfig().backend_isa == kArm64;
}
}  // namespace

void CompleteARM64Allocation(const std::shared_ptr<Module>& module,
                             AllocationPreparation& allocation) {
    if (allocation.TargetPlanning() || !allocation.HasPendingInput()) return;
    ASSERT(module);
    for (;;) {
        TargetPreparation preparation{allocation,
                                      allocation.Workspace(),
                                      module->GetAddressSpace().GetConfig(),
                                      ResolveFeatureSet(module->GetModuleConfig()),
                                      module->GetAddressSpace().GetGuestStateLayout()};
        const auto result =
                preparation.Complete(allocation.InputBlock(), allocation.InputFunction());
        if (result == AllocationPreparationResult::Ready) break;
        ASSERT(result == AllocationPreparationResult::RetryWithoutFPRCoalescing);
        compile::AllocationController::RetryWithoutFPRCoalescing(allocation);
    }
    PerfScope2 perf_release{GetPerfStats2().plan_release};
    compile::AllocationController::Finish(allocation);
}

void TargetPreparation::Finalize(const ir::Block* block,
                                 const ir::HIRFunction* function,
                                 std::span<const ir::Block* const> blocks,
                                 std::span<const PendingGPRBlock> pending,
                                 std::span<const u32> zero_stores,
                                 bool ssa_valid) {
    GPRSMask uniforms{};
    for (u16 reg = 0; reg < 31; ++reg)
        if (guest_layout.uni_gprs.Get(reg)) uniforms.Mark(reg);
    PerfScope2 perf_verify{GetPerfStats2().plan_verify};
    allocation.SealTargetEdits();
    // Bind one semantic description for the complete unit. A manual request
    // with different feature/config inputs is described once for all blocks.
    std::unique_ptr<const compile::FixedValidationInputs> owned_fixed;
    const auto* fixed =
            allocation.PreparedFixedValidation(features, FixedValidationContractKey(features));
    if (!fixed) {
        FixedValidationBuilder builder{features};
        for (const auto& item : pending) {
            builder.BeginBlock(item.block, *item.analysis);
            for (const auto* inst : item.analysis->Instructions()) builder.Describe(*inst);
        }
        owned_fixed = builder.Finish();
        fixed = owned_fixed.get();
    }
    std::vector<EmissionCertificateInput> verification;
    verification.reserve(pending.size());
    for (const auto& item : pending)
        verification.push_back({item.block, item.analysis, item.draft.writes});
    const auto certificates = PrepareEmissionCertificates(
            verification, allocation, allocation.Actions(), *fixed, features, uniforms);
    if (GetSvmConfig().verify_plan) {
        // The optional audit builds its own reference from the sealed IR.
        // Production semantic facts are not an audit oracle.
        AuditGPRAllocation(verification,
                           allocation,
                           allocation.Actions(),
                           features,
                           function,
                           function != nullptr);
    }
    for (auto id : certificates.widths.width) allocation.CertifyWidthElision(id, false);
    for (auto id : certificates.widths.low32) allocation.CertifyWidthElision(id, true);
    for (auto id : certificates.widths.reads) allocation.CertifyHostRead(id);
    allocation.StoreFlagsOnlySpills(certificates.flags_only_spills);
    for (auto id : zero_stores) allocation.CertifyZeroStore(id);
    perf_verify.Stop();
    PerfScope2 perf_edges{GetPerfStats2().plan_edges};
    auto lowered = LowerUnitEdges(function ? nullptr : block,
                                  function,
                                  allocation,
                                  allocation.Actions(),
                                  config,
                                  features);
    lowered.certificate.ssa_valid = ssa_valid;
    auto& facts = allocation.PrepareFacts();
    bool needs_unit_state{};
    for (auto& entry : lowered.actions.blocks) {
        entry.requires_unit_state = function && (facts.RequiresIncomingValues(entry.block) ||
                                                 register_state.HasEntryWidthFacts(entry.block));
        needs_unit_state |= entry.requires_unit_state;
    }
    if (needs_unit_state) {
        const auto& graph = facts.PrepareFunctionCFG();
        const auto blocks = graph.Blocks();
        for (const auto& source : blocks)
            for (const auto next : graph.Successors(source)) {
                if (next == compile::FunctionCFG::MissingBlock) continue;
                const auto* target = lowered.actions.ForBlock(blocks[next].hir->GetBlock());
                if (target && target->requires_unit_state)
                    lowered.actions.unit_state_edges.emplace_back(
                            source.hir->GetBlock()->GetStartLocation().Value(), target->location);
            }
        auto& transfers = lowered.actions.unit_state_edges;
        std::sort(transfers.begin(), transfers.end());
        transfers.erase(std::unique(transfers.begin(), transfers.end()), transfers.end());
    }
    lowered.certificate.entry_state_valid = true;
    allocation.StoreEdges(std::move(lowered));
    const auto* certificate = allocation.Actions().Certificate();
    ASSERT(certificate);
    const auto& options = certificate->options;
    if (certificate->valid) {
        allocation.StoreAtomics(LowerAtomicFallbacks(
                blocks, allocation, options.atomic_lse, options.atomic_memory_base));
        const auto& edges = allocation.Actions().Edges();
        for (const auto& item : pending) {
            if (allocation.Actions().IntegerBranch(item.block)) continue;
            std::array<u64, 2> flags_dead_targets{};
            size_t target_count = 0;
            BackedgeInputs inputs{features};
            // Loop-lazy emission owns a mandatory cycle exit in both unit modes.
            inputs.self_enabled = options.flags_loop_lazy ||
                                  ((options.backedge_latch || options.region_edges) &&
                                   options.backedge_flags);
            inputs.region_enabled = options.flags_region_branch;
            inputs.canonical_carry = options.flags_cfinv && options.flag_m;
            if (!inputs.self_enabled && !inputs.region_enabled) continue;
            if (inputs.region_enabled && edges.active) {
                const auto mark_target = [&](const ir::Terminal& edge) {
                    VisitVariant<void>(edge, [&](const auto& target) {
                        using T = std::decay_t<decltype(target)>;
                        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
                            const auto* found = edges.InternalBlock(target.next.Value());
                            if (found && found->edge_flags.Accepts(EdgeFlagsState::Pending(
                                                 kEdgeNZCVMask,
                                                 EdgeCarryPolarity::Unknown,
                                                 EdgeFlagsProducer::Restore)))
                                flags_dead_targets[target_count++] = target.next.Value();
                        }
                    });
                };
                VisitVariant<void>(item.block->GetTerminal(), [&](const auto& terminal) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(terminal)>,
                                                 ir::terminal::If>) {
                        mark_target(terminal.then_);
                        mark_target(terminal.else_);
                    }
                });
            }
            inputs.flags_dead_targets = std::span{flags_dead_targets}.first(target_count);
            if (auto selected = LowerBackedge(*item.block, inputs))
                allocation.StoreBackedge(item.block, std::move(*selected));
        }
    }
    if (certificate->valid && function) {
        PerfScope2 perf_locations{GetPerfStats2().plan_targets};
        allocation.StoreDeferredLocations(
                PrepareDeferredLocations(*function, facts, allocation, features));
    }
    if (RAShapeProfEnabled()) allocation.RAShape().spill_defs = allocation.SpillCount();
    perf_edges.Stop();
    PerfScope2 perf_seal{GetPerfStats2().plan_seal};
    allocation.EndTargetPlanning();
}

struct Arm64PreparedUnit::Storage {
    bool AllocateFunction(const std::shared_ptr<Module>& module,
                          compile::OptimizedUnit& input,
                          compile::CompileWorkspace& workspace,
                          const PerfFixedCapture2& capture) {
        const auto* function = input.Function();
        const auto& features = input.Features();
        const auto& space = module->GetAddressSpace();
        PerfScope perf_ra{GetPerfStats().regalloc_ns};
        PerfScope2 perf_ra_detail{GetPerfStats2().regalloc_total};
        allocation.emplace(function->MaxInstrCount(),
                           space.GetTrampolines().GetGPRRegs(),
                           space.GetTrampolines().GetFPRRegs(),
                           features);
        actions.emplace(allocation->View());
        AllocationPreparation preparation{*allocation,
                                          input.PrepareFacts(),
                                          workspace,
                                          *actions};
        compile::AllocateFunction(function,
                                  &preparation,
                                  ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                          function, preparation, features),
                                  {.scalar_insert = space.GetConfig().sse_scalar_insert});
        perf_ra_detail.Stop();
        perf_ra.Stop();
        capture.Record(static_cast<unsigned>(input.DecodedBlocks()));
        if (GetSvmConfig().dump_ir)
            SVM_DIAG_FORMAT(Runtime,
                            "[func-compile] {:#x} regalloc-ready\n",
                            function->GetFunction()->GetStartLocation().Value());
        return Complete(module, preparation, input);
    }

    bool AllocateBlock(const std::shared_ptr<Module>& module,
                       compile::OptimizedUnit& input,
                       compile::CompileWorkspace& workspace,
                       std::optional<u32> final_hir_capacity) {
        const auto& space = module->GetAddressSpace();
        const auto& features = input.Features();
        const auto capacity = final_hir_capacity.value_or(input.Block()->MaxInstrId());
        allocation.emplace(capacity,
                           space.GetTrampolines().GetGPRRegs(),
                           space.GetTrampolines().GetFPRRegs(),
                           features);
        actions.emplace(allocation->View());
        AllocationPreparation preparation{*allocation,
                                          input.PrepareFacts(),
                                          workspace,
                                          *actions};
        if (final_hir_capacity && !capacity)
            preparation.SetAllocationUnit(input.Block(), nullptr);
        else {
            PerfScope perf_ra_coarse{GetPerfStats().regalloc_ns};
            PerfScope2 perf_ra{GetPerfStats2().regalloc_total};
            compile::AllocateBlock(input.Block(),
                                   &preparation,
                                   ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                           input.Block(), preparation, features),
                                   space.GetConfig().sse_scalar_insert);
        }
        return Complete(module, preparation, input);
    }

    bool Complete(const std::shared_ptr<Module>& module,
                  AllocationPreparation& preparation,
                  const compile::OptimizedUnit& input) {
        PerfScope perf_codegen{GetPerfStats().codegen_ns};
        PerfScope2 perf_total{GetPerfStats2().codegen_total};
        PerfScope2 perf_complete{GetPerfStats2().codegen_complete};
        return CompleteARM64UnitForEmission(module, preparation, input.Block(), input.Function());
    }

    // The factory transfers sealed IR only after every preparation writer has
    // ended. No semantic analysis cache escapes into the emission result.
    std::optional<compile::CompileUnit> unit;
    std::optional<compile::Allocation> allocation;
    std::optional<UnitActions> actions;
};

std::optional<Arm64PreparedUnit> Arm64PreparedUnit::FromOptimized(
        const std::shared_ptr<Module>& module,
        compile::OptimizedUnit input,
        compile::CompileWorkspace& workspace,
        const PerfFixedCapture2* capture,
        std::optional<u32> final_hir_capacity) {
    if (!input.PrepareFacts().PrepareSSAValidation()) return std::nullopt;
    auto storage = std::make_unique<Storage>();
    bool ready{};
    if (input.Function()) {
        ASSERT(capture);
        ready = storage->AllocateFunction(module, input, workspace, *capture);
    } else
        ready = storage->AllocateBlock(module, input, workspace, final_hir_capacity);
    if (!ready) return std::nullopt;
    storage->unit.emplace(std::move(input).TakeIR());
    return Arm64PreparedUnit{std::move(storage)};
}

Arm64PreparedUnit::Arm64PreparedUnit(std::unique_ptr<Storage> storage) noexcept
        : storage(std::move(storage)) {}
Arm64PreparedUnit::~Arm64PreparedUnit() = default;
Arm64PreparedUnit::Arm64PreparedUnit(Arm64PreparedUnit&&) noexcept = default;
Arm64PreparedUnit& Arm64PreparedUnit::operator=(Arm64PreparedUnit&&) noexcept = default;

const compile::CompileUnit& Arm64PreparedUnit::Unit() const {
    ASSERT(storage);
    ASSERT(storage->unit);
    return *storage->unit;
}
compile::AllocationView Arm64PreparedUnit::Allocation() const {
    ASSERT(storage && storage->allocation);
    return storage->allocation->View();
}
const UnitActions& Arm64PreparedUnit::Actions() const {
    ASSERT(storage && storage->actions);
    return *storage->actions;
}
const ir::Block* Arm64PreparedUnit::Block() const { return Unit().GetBlock(); }
const ir::HIRFunction* Arm64PreparedUnit::Function() const { return Unit().GetFunction(); }
bool Arm64PreparedUnit::ReadyForEmission(const std::shared_ptr<Module>& module) const {
    return storage && storage->unit && storage->unit->IsSealed() && storage->allocation &&
           storage->actions &&
           ARM64UnitReadyForEmission(module, Allocation(), Actions(), Block(), Function());
}

namespace {
compile::CompileOptions PipelineOptions(const std::shared_ptr<Module>& module) {
    const auto& space = module->GetAddressSpace();
    return {.guest_layout = space.GetGuestStateLayout().uniform_size ? &space.GetGuestStateLayout()
                                                                     : nullptr,
            .enabled = module->GetModuleConfig().optimizations,
            .features = ResolveFeatureSet(module->GetModuleConfig()),
            .target = BuilderTarget(),
            .hoist = LoopHoistPolicy(space.GetTrampolines().GetGPRRegs()),
            .read_only_guest = module->GetModuleConfig().read_only};
}
}  // namespace

std::optional<Arm64PreparedUnit> PrepareBlock(const std::shared_ptr<Module>& module,
                                              const IntrusivePtr<ir::Block>& block) {
    if (!IsArm64(module) || !SupportsIR(block.get())) return std::nullopt;
    compile::CompileWorkspaceScope workspace;
    auto input = compile::OptimizeBlock(
            compile::CompileUnit{block}, workspace.Get(), PipelineOptions(module));
    return Arm64PreparedUnit::FromOptimized(module, std::move(input), workspace.Get(), nullptr);
}

std::optional<Arm64PreparedUnit> PrepareFinalBlock(const std::shared_ptr<Module>& module,
                                                   ir::HIRBlock* block) {
    if (!IsArm64(module) || !block || !SupportsIR(block->GetBlock())) return std::nullopt;
    compile::CompileUnit unit{block};
    const auto capacity = ARM64StandaloneHIRCapacity(*block->GetBlock());
    if (!capacity) return std::nullopt;
    unit.SealIR();
    compile::CompileWorkspaceScope workspace;
    auto input = compile::OptimizedUnit::FromFinalIR(std::move(unit),
                                                     ResolveFeatureSet(module->GetModuleConfig()));
    return Arm64PreparedUnit::FromOptimized(
            module, std::move(input), workspace.Get(), nullptr, capacity);
}

std::optional<Arm64PreparedUnit> PrepareFunction(const std::shared_ptr<Module>& module,
                                                 ir::HIRFunction* function) {
    if (!IsArm64(module) || !SupportsIR(function) || function->GetHIRBlocks().size() <= 1 ||
        !compile::FunctionCFGClosed(*function) || !function->GetHIRBlockList().empty())
        return std::nullopt;
    PerfFixedCapture2 capture;
    compile::CompileWorkspaceScope workspace;
    auto input = compile::OptimizeFunction(
            compile::CompileUnit{function}, workspace.Get(), PipelineOptions(module));
    return Arm64PreparedUnit::FromOptimized(module, std::move(input), workspace.Get(), &capture);
}

std::optional<std::vector<Arm64PreparedUnit>> PrepareFunctions(
        const std::shared_ptr<Module>& module, std::span<ir::HIRFunction* const> functions) {
    if (!IsArm64(module) || functions.empty()) return std::nullopt;
    for (size_t i = 0; i < functions.size(); ++i) {
        const auto* function = functions[i];
        if (!SupportsIR(function) || function->GetHIRBlocks().size() <= 1 ||
            !compile::FunctionCFGClosed(*function) || !function->GetHIRBlockList().empty())
            return std::nullopt;
        for (size_t j = 0; j < i; ++j)
            if (functions[j] == function) return std::nullopt;
    }
    PerfFixedCapture2 capture;
    std::vector<compile::CompileUnit> units;
    compile::CompileWorkspaceScope workspace;
    units.reserve(functions.size());
    for (auto* function : functions) units.emplace_back(function);
    auto optimized =
            compile::OptimizeFunctions(std::move(units), workspace.Get(), PipelineOptions(module));
    if (!optimized) return std::nullopt;
    std::vector<Arm64PreparedUnit> prepared;
    prepared.reserve(optimized->size());
    for (auto& input : *optimized) {
        auto result = Arm64PreparedUnit::FromOptimized(
                module, std::move(input), workspace.Get(), &capture);
        if (!result) return std::nullopt;
        prepared.push_back(std::move(*result));
    }
    return prepared;
}

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {
bool ARM64UnitReadyForEmission(const std::shared_ptr<Module>& module,
                               compile::AllocationView allocation,
                               const UnitActions& unit_actions,
                               const ir::Block* block,
                               const ir::HIRFunction* function) {
    if (!module || (!!block == !!function) || !unit_actions.MatchesAllocation(allocation) ||
        !allocation.GPRLocationsFrozen())
        return false;
    const ir::Block* first = block;
    if (function) {
        if (function->GetHIRBlocksRPO().empty()) return false;
        first = function->GetHIRBlocksRPO().front().GetBlock();
    }
    const auto* certificate = unit_actions.Certificate();
    if (!unit_actions.ForBlock(first) || !certificate) return false;
    return certificate->valid && certificate->ssa_valid && certificate->entry_state_valid &&
           certificate->standalone == block && certificate->function == function &&
           certificate->generation == allocation.GPRLocationGeneration() &&
           certificate->allocation_attempt == allocation.AllocationAttempt() &&
           certificate->MatchesTargetInputs(allocation,
                                            module->GetAddressSpace().GetConfig(),
                                            ResolveFeatureSet(module->GetModuleConfig()));
}

bool CompleteARM64UnitForEmission(const std::shared_ptr<Module>& module,
                                  AllocationPreparation& allocation,
                                  const ir::Block* block,
                                  const ir::HIRFunction* function) {
    if (!module || (!!block == !!function)) return false;
    CompleteARM64Allocation(module, allocation);
    if (GetSvmConfig().density_prof) {
        std::fprintf(stderr, "[allocation-retry] fpr=%u\n", allocation.FPRContractRetryCount());
    }
    return ARM64UnitReadyForEmission(module, allocation, allocation.Actions(), block, function);
}

std::optional<u32> ARM64StandaloneHIRCapacity(const ir::Block& block) {
    if (!block.HasTerminal()) return std::nullopt;
    auto& instructions = block.GetInstList();
    if (instructions.size() != block.MaxInstrId()) return std::nullopt;
    std::vector<const ir::Inst*> local;
    local.reserve(instructions.size());
    u32 capacity = 0;
    const auto valid_type = [](ir::ValueType type) {
        return type > ir::ValueType::VOID && type <= ir::ValueType::V256;
    };
    for (auto& inst : instructions) {
        if (inst.Id() != capacity++ || inst.IsPseudoOperation() || inst.HasPseudoOperations() ||
            (inst.HasValue() && !valid_type(inst.ReturnType())))
            return std::nullopt;
        switch (inst.GetOp()) {
            case ir::OpCode::Goto:
            case ir::OpCode::NotGoto:
            case ir::OpCode::BindLabel:
            case ir::OpCode::AddPhi:
                return std::nullopt;
            default:
                break;
        }
        local.push_back(&inst);
    }
    const auto owned = [&](const ir::Inst* inst) {
        return inst && inst->Id() < local.size() && local[inst->Id()] == inst;
    };
    const auto valid_value = [&](ir::Value value, u32 before) {
        return value.Defined() && valid_type(value.Type()) && owned(value.Def()) &&
               value.Id() < before && value.Def()->HasValue() &&
               valid_type(value.Def()->ReturnType());
    };
    for (auto& inst : instructions) {
        bool valid = true;
        inst.ForEachValue([&](ir::Value value) { valid &= valid_value(value, inst.Id()); });
        if (!valid) return std::nullopt;
    }
    const auto& hoist = block.GetLoopHoistMetadata();
    if (hoist.anchors.size() != static_cast<size_t>(hoist.gpr_count) + hoist.const_count ||
        (!hoist.anchors.empty() && !hoist.prefix_end) ||
        (hoist.prefix_end && !owned(hoist.prefix_end)))
        return std::nullopt;
    for (const auto* anchor : hoist.anchors)
        if (!owned(anchor) || !ir::meta::HasValue(ir::GetIRMetaInfo(anchor->GetOp()).return_type) ||
            !valid_type(anchor->ReturnType()))
            return std::nullopt;
    bool values_valid = true;
    const auto entry_node = [](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) return false;
        else if constexpr (std::is_same_v<T, ir::terminal::If>)
            return term.cond.Type() == ir::ValueType::U8;
        else return true;
    };
    const bool terminal_valid = ir::CheckTerminal(block.GetTerminal(),
            [&](ir::Value value) { values_valid &= valid_value(value, capacity); }, entry_node);
    return terminal_valid && values_valid ? std::optional<u32>{capacity} : std::nullopt;
}
}  // namespace swift::runtime::backend::arm64
