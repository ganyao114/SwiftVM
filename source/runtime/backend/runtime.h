//
// Created by 甘尧 on 2024/5/11.
//

#pragma once

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend {
void *TranslateIR(const std::shared_ptr<backend::Module> &module, ir::HIRFunction *function);
void *TranslateIR(const std::shared_ptr<backend::Module> &module, ir::HIRBlock *block);
void *TranslateIR(const std::shared_ptr<backend::Module> &module, const IntrusivePtr<ir::Block> &block);
}
