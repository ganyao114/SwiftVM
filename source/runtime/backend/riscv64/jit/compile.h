#pragma once

#include <span>
#include "runtime/backend/module.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::riscv64 {

void* CompileBlock(const std::shared_ptr<Module>& module, ir::Block* block);
void* CompileFunctions(const std::shared_ptr<Module>& module,
                       std::span<ir::HIRFunction* const> functions);

}  // namespace swift::runtime::backend::riscv64
