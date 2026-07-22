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
    }

    pipeline.AddBlockPass(Optimizations::FlagElimination, [](Block* block) {
        FlagsEliminationPass::Run(block);
    });

    pipeline.AddBlockPass(Optimizations::ConstantFolding, [](Block* block) {
        ConstFoldingPass::Run(block);
    });

    pipeline.AddBlockPass(Optimizations::DeadCodeRemove, [](Block* block) {
        DeadCodeEliminationPass::Run(block);
    });

    return pipeline;
}

}  // namespace swift::runtime::ir
