#pragma once

#include <vector>
#include "runtime/backend/reg_alloc.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"

namespace swift::runtime::backend::arm64 {
std::vector<u32> VerifyZeroStoreValues(const BlockAnalysisIndex& analysis,
        const RegAlloc& allocation, std::vector<const ir::Inst*>* incoming = nullptr);
}
