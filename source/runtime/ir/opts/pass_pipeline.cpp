//
// Created by SwiftVM on 2024/7/1.
//

#include "pass_pipeline.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/ir/opts/local_elimination_pass.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"

namespace swift::runtime::ir {

PassPipeline PassPipeline::BuildDefault(const UniformInfo* uniform_info) {
    PassPipeline pipeline;

    // Order matters: local/uniform elimination first, then flag/const, then dead code last
    pipeline.AddBlockPass(Optimizations::LocalElimination, [](Block* block) {
        LocalEliminationPass::Run(block);
    });

    if (uniform_info) {
        pipeline.AddBlockPass(Optimizations::UniformElimination,
            [uniform_info](Block* block) {
                UniformEliminationPass::Run(block, *uniform_info);
            });
        pipeline.AddFunctionPass(Optimizations::UniformElimination,
            [uniform_info](HIRFunction* function) {
                UniformEliminationPass::Run(function, *uniform_info, true);
            });
    }

    pipeline.AddBlockPass(Optimizations::FlagElimination, [](Block* block) {
        FlagsEliminationPass::Run(block);
    });
    // Function compilation used to run ONLY the uniform-elimination entry
    // (RunFunction skips every entry without a function_pass), so a
    // function-compiled unit kept every dead flag calculation that block
    // compilation removed. Measured on bench_suite kernel_int: 446 host
    // instructions per loop iteration vs 386 for the same guest block under
    // SVM_FUNC_BASE=0.
    pipeline.AddFunctionPass(Optimizations::FlagElimination, [](HIRFunction* function) {
        FlagsEliminationPass::Run(function);
    });

    pipeline.AddBlockPass(Optimizations::ConstantFolding, [](Block* block) {
        ConstFoldingPass::Run(block);
    });
    // Same wiring gap d42bb4f fixed for flag elimination: without a function
    // entry the pass simply never runs on a function-compiled unit.
    pipeline.AddFunctionPass(Optimizations::ConstantFolding, [](HIRFunction* function) {
        ConstFoldingPass::Run(function);
    });

    pipeline.AddBlockPass(Optimizations::DeadCodeRemove, [](Block* block) {
        DeadCodeEliminationPass::Run(block);
    });
    // Must stay last: it collects the def chains that flag elimination just
    // orphaned.
    pipeline.AddFunctionPass(Optimizations::DeadCodeRemove, [](HIRFunction* function) {
        DeadCodeEliminationPass::Run(function);
    });

    return pipeline;
}

}  // namespace swift::runtime::ir
