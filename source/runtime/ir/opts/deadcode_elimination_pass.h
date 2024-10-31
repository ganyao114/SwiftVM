//
// Created by 甘尧 on 2024/6/24.
//

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

class DeadCodeEliminationPass {
public:
    static void Run(HIRBuilder* hir_builder);
    static void Run(HIRFunction* hir_function);
    static void Run(Block* block);
};

}  // namespace swift::runtime::ir
