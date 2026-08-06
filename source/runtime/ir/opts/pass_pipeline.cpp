//
// Created by SwiftVM on 2024/7/1.
//

#include "pass_pipeline.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"
#include "runtime/ir/opts/uniform_store_sink_pass.h"

namespace swift::runtime::ir {

PassPipeline PassPipeline::BuildDefault(const UniformInfo* uniform_info) {
    PassPipeline pipeline;

    // Order matters: uniform elimination first, then flag/const, then dead code last
    if (uniform_info) {
        pipeline.AddBlockPass(Optimizations::UniformElimination,
            [uniform_info](Block* block, const FeatureSet& features) {
                if (!features.uniform_elim) return;
                UniformEliminationPass::Run(block, *uniform_info, features);
            });
        pipeline.AddFunctionPass(Optimizations::UniformElimination,
            [uniform_info](HIRFunction* function, const FeatureSet& features) {
                if (!features.uniform_elim) return;
                UniformEliminationPass::Run(function, *uniform_info, true, features);
            });
        pipeline.AddBlockPass(Optimizations::XmmFaultSink,
            [uniform_info](Block* block, const FeatureSet& features) {
                if (features.xmm_fault_sink) {
                    UniformStoreSinkPass::Run(block, *uniform_info);
                }
                UniformEliminationPass::FinalizeStaticFPRMappings(block, *uniform_info);
            });
        pipeline.AddFunctionPass(Optimizations::XmmFaultSink,
            [uniform_info](HIRFunction* function, const FeatureSet& features) {
                if (features.xmm_fault_sink) {
                    UniformStoreSinkPass::Run(function, *uniform_info);
                }
                UniformEliminationPass::FinalizeStaticFPRMappings(function, *uniform_info);
            });
    }

    pipeline.AddBlockPass(Optimizations::FlagElimination,
                          [](Block* block, const FeatureSet& features) {
        FlagsEliminationPass::Run(block, nullptr, features);
    });
    // Function compilation used to run ONLY the uniform-elimination entry
    // (RunFunction skips every entry without a function_pass), so a
    // function-compiled unit kept every dead flag calculation that block
    // compilation removed. Measured on bench_suite kernel_int: 446 host
    // instructions per loop iteration vs 386 for the same guest block under
    // SVM_FUNC_BASE=0.
    pipeline.AddFunctionPass(Optimizations::FlagElimination,
                             [](HIRFunction* function, const FeatureSet& features) {
        FlagsEliminationPass::Run(function, features);
    });

    pipeline.AddBlockPass(Optimizations::ConstantFolding,
                          [](Block* block, const FeatureSet& features) {
        ConstFoldingPass::Run(block, features);
    });
    // Same wiring gap d42bb4f fixed for flag elimination: without a function
    // entry the pass simply never runs on a function-compiled unit.
    pipeline.AddFunctionPass(Optimizations::ConstantFolding,
                             [](HIRFunction* function, const FeatureSet& features) {
        ConstFoldingPass::Run(function, features);
    });

    pipeline.AddBlockPass(Optimizations::DeadCodeRemove, [](Block* block, const FeatureSet&) {
        DeadCodeEliminationPass::Run(block);
    });
    // Must stay last: it collects the def chains that flag elimination just
    // orphaned.
    pipeline.AddFunctionPass(Optimizations::DeadCodeRemove,
                             [](HIRFunction* function, const FeatureSet&) {
        DeadCodeEliminationPass::Run(function);
    });

    return pipeline;
}

}  // namespace swift::runtime::ir
