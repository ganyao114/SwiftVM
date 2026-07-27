//
// Created by 甘尧 on 2024/6/24.
//

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class DeadCodeEliminationPass {
public:
    static void Run(HIRBuilder* hir_builder);
    static void Run(HIRFunction* hir_function);
    // See FlagsEliminationPass::Run for why the function path needs the
    // owning HIRFunction: deletions must go through EraseInst.
    static void Run(Block* block, HIRFunction* hir_function = nullptr);
};

}  // namespace swift::runtime::ir
