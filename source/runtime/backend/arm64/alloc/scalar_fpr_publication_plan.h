#pragma once
#include <map>
#include <vector>
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::arm64 {
class BlockAnalysisIndex;
struct ScalarFPRPublication {
    ir::Inst* low_store{};
    ir::Inst* high_store{};
    ir::Inst* zero{};
    ir::Inst* load_extension{};
    u16 target{};
};
struct ScalarFPRPublicationPlan {
    std::map<ir::Inst*, ScalarFPRPublication> loads, values;
    std::vector<ir::Inst*> covered;
};
ScalarFPRPublicationPlan BuildScalarFPRPublicationPlan(ir::Block* block, const RegAlloc& allocation,
        const BlockAnalysisIndex* analysis = nullptr);
}
