#include "compile.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <fmt/format.h>
#include "translator.h"
#include "function.h"
#include "uniform_forward.h"
#include "flags_liveness.h"
#include "cfg_uniform_forward.h"
#include "cfg_state_promote.h"
#include "runtime/backend/address_space.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"

namespace swift::runtime::backend::x64 {

namespace {
bool UseScalarCache(const Module& module) {
    return ResolveFeatureSet(module.GetModuleConfig()).x64_scalar_cache;
}
bool UseCfgAllocation(const Module& module) {
    return ResolveFeatureSet(module.GetModuleConfig()).x64_cfg_allocation;
}
bool UseFlagsResidency(const Module& module) {
    return ResolveFeatureSet(module.GetModuleConfig()).x64_flags_residency;
}
bool UseNativeCfChains(const Module& module) {
    return ResolveFeatureSet(module.GetModuleConfig()).x64_native_cf_chains;
}
bool UseVectorCfg(const Module& module) {
    return ResolveFeatureSet(module.GetModuleConfig()).x64_vector_cfg;
}
void ReportEmission(const Translator& translator, const ir::Block* block) {
    if (!GetSvmConfig().func_stats) return;
    const auto& stats = translator.Statistics();
    fmt::print(stderr, "[x64-emission] pc={:#x} scalar_cache_reads={} scalar_cache_writes={} code_bytes={} frame_bytes={} coalesced_binary={}\n",
               block->GetStartLocation().Value(), stats.scalar_cache_read_sites,
               stats.scalar_cache_write_sites, translator.getSize(), translator.FrameSize(),
               stats.scalar_coalesced_binary_sites);
    fmt::print(stderr, "[x64-scalar-home] pc={:#x} omitted_high_word_stores={}\n",
               block->GetStartLocation().Value(), stats.scalar_high_word_store_omissions);
    fmt::print(stderr, "[x64-flags-residency] pc={:#x} enabled={} state_loads={} state_stores={} register_reads={} register_writes={} direct_merges={} direct_updates={}\n",
               block->GetStartLocation().Value(), translator.FlagsResident() ? 1 : 0, stats.flags_state_load_sites,
               stats.flags_state_store_sites, stats.flags_resident_read_sites,
               stats.flags_resident_write_sites, stats.flags_resident_merge_sites,
               stats.flags_resident_update_sites);
    const auto& allocation = translator.CfgPlan();
    fmt::print(stderr, "[x64-scalar-unary-mask] pc={:#x} enabled={} omissions={}\n",
               block->GetStartLocation().Value(), translator.ScalarUnaryMaskElisionEnabled() ? 1 : 0,
               stats.scalar_unary_mask_omissions);
    fmt::print(stderr, "[x64-vector-cfg] pc={:#x} enabled={} colors={} call_frame_bytes={} cache_reads={} cache_writes={} phi_publications={} helper_saves={} helper_restores={} legacy_homes_retained={}\n",
               block->GetStartLocation().Value(), translator.VectorCfgAllocationEnabled() ? 1 : 0,
               translator.VectorPlan().UsedRegisters().size(), translator.VectorCallFrameBytes(),
               stats.vector_cfg_cache_read_sites, stats.vector_cfg_cache_write_sites,
               stats.vector_cfg_phi_publication_sites, stats.vector_cfg_helper_save_sites,
               stats.vector_cfg_helper_restore_sites, stats.vector_cfg_legacy_home_retentions);
    fmt::print(stderr, "[x64-vector-analysis] pc={:#x} requested={} enabled={} rejection={} exhausted_limit={} nodes={} work={} pair_work={} storage_entries={} fixed_point_passes={}\n",
               block->GetStartLocation().Value(), stats.vector_cfg_requested ? 1 : 0,
               stats.vector_cfg_plan_enabled ? 1 : 0, stats.vector_cfg_rejection_reason,
               stats.vector_cfg_budget_limit, stats.vector_cfg_analysis_nodes, stats.vector_cfg_analysis_work,
               stats.vector_cfg_pair_work, stats.vector_cfg_interference_storage_entries, stats.vector_cfg_fixed_point_passes);
    fmt::print(stderr, "[x64-native-cf] pc={:#x} enabled={} forwards={} merge_omissions={} state_bt_omissions={} resident_bt_omissions={} polarity_cmc={}\n",
               block->GetStartLocation().Value(), translator.NativeCfChainsEnabled() ? 1 : 0,
               stats.native_cf_forward_sites, stats.native_cf_merge_omissions,
               stats.native_cf_state_bt_omissions, stats.native_cf_resident_bt_omissions,
               stats.native_cf_polarity_cmc_sites);
    fmt::print(stderr, "[x64-scalar-shift-immediate] pc={:#x} enabled={} sites={} zero_sites={}\n",
               block->GetStartLocation().Value(), translator.ScalarShiftImmediatesEnabled() ? 1 : 0,
               stats.scalar_shift_immediate_sites, stats.scalar_shift_zero_sites);
    fmt::print(stderr, "[x64-scalar-unary-coalescing] pc={:#x} enabled={} sites={}\n",
               block->GetStartLocation().Value(), translator.ScalarUnaryCoalescingEnabled() ? 1 : 0,
               stats.scalar_coalesced_unary_sites);
    fmt::print(stderr, "[x64-flags-carry] pc={:#x} state_bt_reads={} resident_bt_reads={}\n",
               block->GetStartLocation().Value(), stats.flags_state_carry_read_sites,
               stats.flags_resident_carry_read_sites);
    const auto& construction = allocation.ConstructionStats();
    fmt::print(stderr, "[x64-cfg-loop-cost] pc={:#x} enabled={} natural_loops={} max_depth={}\n",
               block->GetStartLocation().Value(), allocation.LoopWeightedCostsEnabled() ? 1 : 0,
               construction.natural_loops, construction.max_loop_depth);
    fmt::print(stderr, "[x64-cfg-interference] pc={:#x} dense={} sparse_fallback={} dense_bytes={} clique_calls={} instructions={}\n",
               block->GetStartLocation().Value(), construction.dense_used ? 1 : 0,
               construction.sparse_fallback ? 1 : 0, construction.dense_bytes, construction.clique_calls,
               allocation.Instructions().size());
    const auto& preparation = translator.PreparationStats();
    const auto& flags_policy = translator.FlagsDecision();
    fmt::print(stderr, "[x64-flags-policy] pc={:#x} selected={} reason={} allocation_builds={} allocation_recolors={} weighted_reads={} weighted_writes={} carry_reads={} helper_calls={} overhead={} scalar_lost_benefit={} net_savings={}\n",
               block->GetStartLocation().Value(), flags_policy.selected ? 1 : 0,
               static_cast<unsigned>(flags_policy.reason), preparation.flags_policy_allocation_builds,
               preparation.flags_policy_allocation_recolors,
               flags_policy.weighted_flag_reads, flags_policy.weighted_flag_writes,
               flags_policy.weighted_carry_reads, flags_policy.weighted_helper_calls,
               flags_policy.resident_overhead, flags_policy.scalar_lost_benefit, flags_policy.net_savings);
    fmt::print(stderr, "[x64-cfg-preparation] pc={:#x} full_allocation_builds={} reused_final_allocation={}\n",
               block->GetStartLocation().Value(), preparation.full_allocation_builds,
               preparation.reused_final_allocation ? 1 : 0);
    const auto& frame = translator.FramePlan();
    fmt::print(stderr, "[x64-scalar-rematerialization] pc={:#x} enabled={} omitted_homes={} definitions={} reads={}\n",
               block->GetStartLocation().Value(), frame.RematerializationEnabled() ? 1 : 0,
               frame.Statistics().omitted_rematerialized, stats.scalar_rematerialized_definition_sites,
               stats.scalar_rematerialized_read_sites);
    fmt::print(stderr, "[x64-scalar-binary-immediate] pc={:#x} enabled={} sites={} rematerialized={}\n",
               block->GetStartLocation().Value(), translator.ScalarBinaryImmediatesEnabled() ? 1 : 0,
               stats.scalar_binary_immediate_sites, stats.scalar_binary_rematerialized_immediate_sites);
    if (frame.Enabled()) {
        const auto& layout = frame.Statistics();
        fmt::print(stderr, "[x64-sparse-frame] pc={:#x} homes={} omitted_control={} omitted_register={} omitted_unused={} home_bytes={} physical_homes={} reused_scalar={} shared_scalar_slots={}\n",
                   block->GetStartLocation().Value(), layout.retained_homes, layout.omitted_control,
                   layout.omitted_register, layout.omitted_unused, layout.home_bytes, layout.home_count,
                   layout.reused_scalar_homes, layout.shared_scalar_home_slots);
    }
    const auto assigned = std::count_if(allocation.Allocations().begin(), allocation.Allocations().end(),
                                       [](const auto& value) { return value.slot.has_value(); });
    fmt::print(stderr, "[x64-cfg-allocation] pc={:#x} enabled={} blocks={} edges={} assigned_values={} home_values={} registers={}\n",
               block->GetStartLocation().Value(), allocation.Enabled() ? 1 : 0,
               allocation.Blocks().size(), allocation.Edges().size(), assigned,
               allocation.Allocations().size() - assigned, allocation.PhysicalRegisters().size());
    fmt::print(stderr, "[x64-phi-transport] pc={:#x} direct={}\n",
               block->GetStartLocation().Value(), stats.direct_phi_copy_sites);
}
void Prepare(const Module& module, ir::Block* block, ir::HIRFunction* hir = nullptr) {
    const auto& config = module.GetModuleConfig();
    if (config.HasOpt(Optimizations::UniformElimination))
        ForwardUniformLoads(block, module.GetAddressSpace().GetConfig().uniform_buffer_size);
    if (config.HasOpt(Optimizations::ConstantFolding))
        ir::ConstFoldingPass::Run(block, ResolveFeatureSet(config));
    if (config.HasOpt(Optimizations::DeadCodeRemove))
        ir::DeadCodeEliminationPass::Run(block, hir);
    if (config.HasOpt(Optimizations::FlagElimination)) {
        const auto stats = NarrowCarryChainFlags(block, module.GetAddressSpace().GetConfig().uniform_buffer_size);
        if (GetSvmConfig().func_stats)
            fmt::print(stderr, "[x64-flags-liveness] pc={:#x} full={} narrowed={} barriers={}\n",
                       block->GetStartLocation().Value(), stats.full_publications,
                       stats.narrowed_to_carry, stats.barriers);
    }
    if (!hir) block->ReIdInstr();
    u64 size{};
    for (auto& inst : block->GetInstList())
        if (inst.GetOp() == ir::OpCode::AdvancePC) size += inst.GetArg<ir::Imm>(0).Get();
    if (size) block->SetEndLocation(block->GetStartLocation() + size);
    else if (block->GetEndLocation().Value() <= block->GetStartLocation().Value())
        block->SetEndLocation(block->GetStartLocation() + 1);
}

void* Publish(const std::shared_ptr<Module>& module, ir::Block* block,
              const Translator& translator, ir::AddressNode* owner = nullptr) {
    auto [id, buffer] = module->AllocCodeCache(translator.getSize());
    if (id == INVALID_CACHE_ID) return nullptr;
    std::memcpy(buffer.rw_data, translator.getCode(), translator.getSize());
    buffer.Flush();
    auto& cached = block->GetJitCache();
    cached.cache_id = id;
    cached.offset_in = buffer.offset;
    cached.cache_size = buffer.size;
    cached.jit_state = JitState::Cached;
    module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                         block->GetStartLocation().Value(), buffer.exec_data,
                         buffer.exec_data + translator.RecoveryOffset());
    module->RetainCodeIR(buffer.exec_data, owner ? owner : block);
    module->GetAddressSpace().PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    if (!module->GetModuleConfig().read_only) {
        module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner ? owner : block,
                block->GetStartLocation().Value(), block->GetEndLocation().Value());
        for (const auto& dependency : block->GetGuestCodeDependencies())
            module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner ? owner : block,
                    dependency.start.Value(), dependency.end.Value());
    }
    return buffer.exec_data;
}
}

void* CompileBlock(const std::shared_ptr<Module>& module, ir::Block* block) {
    if (block->GetJitCache().jit_state == JitState::Cached)
        return module->GetJitCache(block->GetJitCache());
    Prepare(*module, block);
    const auto policy = ResolveFeatureSet(module->GetModuleConfig());
    if (policy.x64_cfg_allocation && policy.x64_flags_profitability) {
        backend::FunctionPlan plan;
        plan.body = block;
        auto translator = Translator::PrepareFunction(plan, HostFeatures::Detect(),
            &module->GetAddressSpace().GetConfig(), policy.x64_scalar_cache,
            policy.x64_cfg_allocation, policy.x64_flags_residency,
            {.profitable_flags_residency = policy.x64_flags_profitability,
             .native_cf_chains = policy.x64_native_cf_chains, .vector_cfg_allocation = policy.x64_vector_cfg});
        translator->Translate();
        ReportEmission(*translator, block);
        return Publish(module, block, *translator);
    }
    Translator translator{block, HostFeatures::Detect(), &module->GetAddressSpace().GetConfig(), nullptr,
                          UseScalarCache(*module), ExecutionMode::Ordinary,
                          UseCfgAllocation(*module), UseFlagsResidency(*module),
                          true, true, true, true, true, true, true, UseNativeCfChains(*module), UseVectorCfg(*module)};
    translator.Translate();
    ReportEmission(translator, block);
    return Publish(module, block, translator);
}

void* CompileFunctions(const std::shared_ptr<Module>& module,
                       std::span<ir::HIRFunction* const> functions) {
    void* first{};
    for (auto* function : functions) {
        std::vector<ir::Block*> ordered;
        std::vector<ir::HIRBlock*> hir_ordered;
        const auto start = function->GetFunction()->GetStartLocation();
        for (auto* hir : function->GetHIRBlocks())
            if (hir && hir->GetBlock()->GetStartLocation() == start &&
                (hir->GetBlock()->HasTerminal() || !hir->GetInstList().empty())) {
                ordered.push_back(hir->GetBlock()); hir_ordered.push_back(hir);
            }
        for (auto* hir : function->GetHIRBlocks())
            if (hir && hir->GetBlock()->GetStartLocation() != start &&
                hir->GetBlock()->GetStartLocation() != ir::Location::INVALID && hir->GetBlock()->HasTerminal())
                { ordered.push_back(hir->GetBlock()); hir_ordered.push_back(hir); }
        if (ordered.empty()) throw std::runtime_error("x64 function has no root block");
        for (auto* block : ordered) Prepare(*module, block, function);
        u64 end = start.Value();
        for (auto* block : ordered) end = std::max(end, block->GetEndLocation().Value());
        function->GetFunction()->SetEndLocation(ir::Location{end});
        if (UseCfgAllocation(*module) || NeedsFunctionFrame(hir_ordered)) {
            auto plan = backend::FunctionPlan::Build(function, hir_ordered);
            const bool cfg_allocation = UseCfgAllocation(*module);
            const bool forward = cfg_allocation && module->GetModuleConfig().HasOpt(Optimizations::UniformElimination);
            auto prepared = Translator::PrepareFunction(plan, HostFeatures::Detect(),
                                   &module->GetAddressSpace().GetConfig(), UseScalarCache(*module),
                                   cfg_allocation, UseFlagsResidency(*module),
                                   {.uniform_forwarding = forward, .state_promotion = forward,
                                    .profitable_flags_residency = ResolveFeatureSet(module->GetModuleConfig()).x64_flags_profitability,
                                    .native_cf_chains = UseNativeCfChains(*module), .vector_cfg_allocation = UseVectorCfg(*module)});
            auto& translator = *prepared;
            if (forward && GetSvmConfig().func_stats) {
                const auto& promoted = translator.StatePromotionStats();
                fmt::print(stderr, "[x64-cfg-state] pc={:#x} accepted={} entry_seeds={} state_phis={} forwarded={} stores_retained={} candidate_lanes={} rejected_dirty_loop_lanes={}\n",
                           start.Value(), promoted.accepted ? 1 : 0, promoted.entry_seeds,
                           promoted.state_phis, promoted.loads_forwarded, promoted.stores_retained,
                           promoted.candidate_lanes, promoted.rejected_dirty_loop_lanes);
                const auto& stats = translator.UniformForwardStats();
                fmt::print(stderr, "[x64-cfg-uniform] pc={:#x} accepted={} forwarded={} stores_retained={} policy_enabled={} candidates={} scalar_candidates={} rejected_scalar_sources={} allocation_rounds={} allocation_limit_reached={} final_scalar_sources_allocated={}\n",
                           start.Value(), stats.accepted ? 1 : 0, stats.loads_forwarded, stats.stores_retained,
                           stats.allocation_policy_enabled ? 1 : 0, stats.candidate_loads, stats.scalar_candidates,
                           stats.rejected_scalar_sources, stats.allocation_rounds,
                           stats.allocation_limit_reached ? 1 : 0, stats.final_scalar_sources_allocated ? 1 : 0);
            }
            translator.Translate(); // Fully verify/emit before ownership or entry publication.
            ReportEmission(translator, plan.body.get());
            if (translator.getSize() > UINT32_MAX) throw std::runtime_error("x64 function CFG code exceeds cache size limit");
            auto [id, buffer] = module->AllocCodeCache(translator.getSize());
            if (id == INVALID_CACHE_ID) return nullptr;
            IntrusivePtr<ir::Function> owner_guard;
            bool published{};
            auto* root = ordered.front();
            try {
                std::memcpy(buffer.rw_data, translator.getCode(), translator.getSize());
                buffer.Flush();
                module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                        start.Value(), buffer.exec_data, buffer.exec_data + translator.RecoveryOffset());
                if (!module->Push(function->GetFunction())) throw std::runtime_error("failed to publish x64 function CFG");
                function->ReleaseFunctionOwnership();
                published = true;
                owner_guard = function->GetFunction();
                module->RetainCodeIR(buffer.exec_data, owner_guard.get());
                module->RetainCodeIR(buffer.exec_data, plan.body.get());
                auto& cached = root->GetJitCache();
                cached.cache_id = id; cached.offset_in = buffer.offset;
                cached.cache_size = buffer.size; cached.jit_state = JitState::Cached;
                owner_guard->GetJitCache() = cached;
                // Interior labels expect the shared frame/phi predecessor
                // state, so only the function root is a public ABI entry.
                module->GetAddressSpace().PushCodeCache(start, buffer.exec_data);
                if (!module->GetModuleConfig().read_only) for (auto* block : ordered) {
                    module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner_guard.get(),
                            block->GetStartLocation().Value(), block->GetEndLocation().Value());
                    for (const auto& dependency : block->GetGuestCodeDependencies())
                        module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner_guard.get(),
                                dependency.start.Value(), dependency.end.Value());
                }
                if (!first) first = buffer.exec_data;
            } catch (...) {
                if (published) {
                    module->GetAddressSpace().PushCodeCache(start, nullptr);
                    root->GetJitCache() = JitCache{};
                    owner_guard->GetJitCache() = JitCache{};
                    module->Remove(owner_guard.get());
                }
                module->ReclaimCode(buffer.exec_data);
                throw;
            }
            continue;
        }
        // Complete code generation before publishing; unsupported cross-block
        // SSA must leave the function eligible for flat-block fallback.
        std::vector<std::unique_ptr<Translator>> translators;
        for (auto* block : ordered) {
            auto translator = std::make_unique<Translator>(block, HostFeatures::Detect(),
                                                           &module->GetAddressSpace().GetConfig(), nullptr,
                                                           UseScalarCache(*module), ExecutionMode::Ordinary,
                                                           UseCfgAllocation(*module), UseFlagsResidency(*module),
                                                           true, true, true, true, true, true, true, UseNativeCfChains(*module), UseVectorCfg(*module));
            translator->Translate();
            ReportEmission(*translator, block);
            translators.emplace_back(std::move(translator));
        }
        u64 total_size{};
        for (const auto& translator : translators) total_size += translator->getSize();
        if (total_size > UINT32_MAX) throw std::runtime_error("x64 function code exceeds cache size limit");
        auto [id, buffer] = module->AllocCodeCache(total_size);
        if (id == INVALID_CACHE_ID) return nullptr;
        bool published{};
        IntrusivePtr<ir::Function> owner_guard;
        try {
            u32 offset{};
            for (u32 index = 0; index < ordered.size(); ++index) {
                const auto& translator = *translators[index];
                std::memcpy(buffer.rw_data + offset, translator.getCode(), translator.getSize());
                module->AddFaultEntry(buffer.exec_data + offset, buffer.exec_data + offset + translator.getSize(),
                        ordered[index]->GetStartLocation().Value(), buffer.exec_data,
                        buffer.exec_data + offset + translator.RecoveryOffset());
                offset += translator.getSize();
            }
            buffer.Flush();
            if (!module->Push(function->GetFunction())) throw std::runtime_error("failed to publish x64 function");
            function->ReleaseFunctionOwnership();
            published = true;
            owner_guard = function->GetFunction();
            module->RetainCodeIR(buffer.exec_data, owner_guard.get());
            offset = 0;
            for (u32 index = 0; index < ordered.size(); ++index) {
                auto* block = ordered[index];
                auto& cached = block->GetJitCache();
                cached.cache_id = id; cached.offset_in = buffer.offset + offset;
                cached.cache_size = translators[index]->getSize(); cached.jit_state = JitState::Cached;
                module->GetAddressSpace().PushCodeCache(block->GetStartLocation(), buffer.exec_data + offset);
                offset += translators[index]->getSize();
            }
            function->GetFunction()->GetJitCache() = ordered.front()->GetJitCache();
            function->GetFunction()->GetJitCache().cache_size = total_size;
            for (auto* block : ordered) {
                if (!module->GetModuleConfig().read_only) {
                    module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner_guard.get(),
                            block->GetStartLocation().Value(), block->GetEndLocation().Value());
                    for (const auto& dependency : block->GetGuestCodeDependencies())
                        module->GetAddressSpace().GetSmcTracker().RegisterNode(module, owner_guard.get(),
                                dependency.start.Value(), dependency.end.Value());
                }
            }
            if (!first) first = buffer.exec_data;
        } catch (...) {
            if (published) {
                for (auto* block : ordered) {
                    module->GetAddressSpace().PushCodeCache(block->GetStartLocation(), nullptr);
                    block->GetJitCache() = JitCache{};
                }
                owner_guard->GetJitCache() = JitCache{};
                module->Remove(owner_guard.get());
            }
            module->ReclaimCode(buffer.exec_data);
            throw;
        }
    }
    return first;
}

}  // namespace swift::runtime::backend::x64
