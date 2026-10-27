#include "pipeline.h"
#include "runtime/compile/workspace.h"

#include <type_traits>

#include "runtime/common/perf_stats.h"
#include "runtime/common/signal_diagnostic.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/compile/passes/value_worklist.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {
namespace {

template <typename Unit> void RunResidualPasses(Unit* unit,
                                                CompileWorkspace& workspace,
                                                const GuestStateLayout* guest_layout,
                                                Optimizations enabled,
                                                const FeatureSet& features,
                                                const BuilderCapabilities& target) {
    auto& stats = GetPerfStats2();
    const bool cleanup = True(enabled & Optimizations::DeadCodeRemove);
    auto rewrite = [&] {
        if constexpr (std::is_same_v<Unit, ir::Block>) {
            return RewriteContext{unit, workspace, nullptr, cleanup};
        } else {
            return RewriteContext{unit, workspace, cleanup};
        }
    }();
    if (guest_layout && True(enabled & Optimizations::UniformElimination)) {
        PerfScope2 perf{stats.pass_uniform};
        if (features.uniform_elim) {
            CleanupState(rewrite, *guest_layout, features);
        }
    }
    if (guest_layout && True(enabled & Optimizations::XmmFaultSink)) {
        PerfScope2 perf{stats.pass_uniform};
        if (features.xmm_fault_sink) {
            SinkStateStores(rewrite, *guest_layout);
        }
        FinalizeStateMappings(rewrite, *guest_layout);
    }
    if (True(enabled & Optimizations::FlagElimination)) {
        PerfScope2 perf{stats.pass_flags};
        EliminateFlags(rewrite, features);
    }
    std::optional<ValueWorklist> values;
    if (True(enabled & Optimizations::ConstantFolding)) {
        PerfScope2 perf{stats.pass_const};
        BeginValueCleanup(values, rewrite, features, target, ValueCleanup::All);
    }
    for (;;) {
        if (cleanup) {
            PerfScope2 perf{stats.pass_dce};
            rewrite.CollectDead();
        }
        if (True(enabled & Optimizations::FlagElimination) && rewrite.HasFlagChanges()) {
            PerfScope2 perf{stats.pass_flags};
            EliminateFlags(rewrite, features);
        }
        if (!values || !values->Pending()) break;
        PerfScope2 perf{stats.pass_const};
        values->Drain();
    }
}

}  // namespace

namespace {
size_t OptimizeFunctionIR(CompileUnit& unit,
                          CompileWorkspace& workspace,
                          const CompileOptions& options) {
    auto* function = unit.EditFunction();
    const auto start = function->GetFunction()->GetStartLocation().Value();
    PerfScope2 perf_prepare{GetPerfStats2().publish_prepare};
    const auto decoded_blocks = PrepareFunctionGuestRanges(function);
    perf_prepare.Stop();
    PerfScope perf_rpo{GetPerfStats().rpo_ns};
    PerfScope2 perf_compute_rpo{GetPerfStats2().compute_rpo};
    function->ComputeRPO();
    perf_compute_rpo.Stop();
    PerfScope2 perf_id_pre{GetPerfStats2().id_rpo_pre};
    function->IdByRPO();
    perf_id_pre.Stop();
    perf_rpo.Stop();
    if (GetSvmConfig().dump_ir) SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} rpo-ready\n", start);
    PerfScope perf_opt{GetPerfStats().opt_ns};
    {
        PerfScope2 perf{GetPerfStats2().pass_total};
        RunResidualPasses(function,
                          workspace,
                          options.guest_layout,
                          options.enabled,
                          options.features,
                          options.target);
    }
    if (options.guest_layout &&
        (options.features.loop_gpr_hoist || options.features.loop_const_hoist)) {
        ASSERT(options.hoist.describe);
        PerfScope2 perf_hoist{GetPerfStats2().pass_loop_hoist};
        HoistLoopInvariants(function, *options.guest_layout, unit, options.hoist, options.features);
    }
    perf_opt.Stop();
    PerfScope perf_rpo_post{GetPerfStats().rpo_ns};
    PerfScope2 perf_id_post{GetPerfStats2().id_rpo_post};
    function->IdByRPO();
    perf_id_post.Stop();
    perf_rpo_post.Stop();
    if (GetSvmConfig().dump_ir)
        SVM_DIAG_FORMAT(Runtime, "[func-compile] {:#x} opts-ready\n", start);
    return decoded_blocks;
}
}  // namespace

OptimizedUnit OptimizeBlock(CompileUnit unit,
                            CompileWorkspace& workspace,
                            const CompileOptions& options) {
    auto* block = unit.EditBlock();
    PerfScope perf_opt{GetPerfStats().opt_ns};
    {
        PerfScope2 perf{GetPerfStats2().pass_total};
        RunResidualPasses(block,
                          workspace,
                          options.guest_layout,
                          options.enabled,
                          options.features,
                          options.target);
    }
    {
        PerfScope2 perf{GetPerfStats2().reid_block};
        block->ReIdInstr();
    }
    if (!options.read_only_guest) {
        u64 bytes{};
        for (const auto& inst : block->GetInstList())
            if (inst.GetOp() == ir::OpCode::AdvancePC) bytes += inst.GetArg<ir::Imm>(0).Get();
        if (bytes) block->SetEndLocation(block->GetStartLocation() + bytes);
    }
    unit.SealIR();
    return OptimizedUnit::FromFinalIR(std::move(unit), options.features);
}

OptimizedUnit OptimizeFunction(CompileUnit unit,
                               CompileWorkspace& workspace,
                               const CompileOptions& options) {
    const auto decoded = OptimizeFunctionIR(unit, workspace, options);
    unit.SealIR();
    return OptimizedUnit::FromFinalIR(std::move(unit), options.features, decoded);
}

std::optional<std::vector<OptimizedUnit>> OptimizeFunctions(std::vector<CompileUnit> units,
                                                            CompileWorkspace& workspace,
                                                            const CompileOptions& options) {
    if (units.empty()) return std::nullopt;
    std::vector<size_t> decoded;
    StackVector<CompileUnit*, 4> parents;
    decoded.reserve(units.size());
    for (auto& unit : units) {
        decoded.push_back(OptimizeFunctionIR(unit, workspace, options));
        parents.push_back(&unit);
    }
    if (!CompileUnit::MergeFunctions({parents.data(), parents.size()})) return std::nullopt;
    for (auto& unit : units) unit.SealIR();
    std::vector<OptimizedUnit> result;
    result.reserve(units.size());
    for (size_t i = 0; i < units.size(); ++i)
        result.push_back(
                OptimizedUnit::FromFinalIR(std::move(units[i]), options.features, decoded[i]));
    return result;
}

}  // namespace swift::runtime::compile
