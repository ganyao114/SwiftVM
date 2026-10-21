#pragma once
#include <map>
#include <vector>
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::arm64 {
struct GPRBlockPlan;
struct GPRResidenceFacts;
class BlockAnalysisIndex;
struct MemoryLoweringOptions {
    bool narrow_fuse{}, address_tie{}, biased{}, rcpc{};
};
struct NarrowLoadAction {
    ir::Inst* extension{};
    bool direct{}, sign{};
    u8 access_bytes{}, result_bytes{};
};
struct MemoryWritebackAction {
    ir::Inst* update{};
    u16 base{};
    s64 offset{};
    u8 access_bytes{};
    u32 consumed_value{UINT32_MAX};
};
struct MemoryOffsetAction {
    ir::Value base{};
    s64 offset{};
};
struct MemoryLoweringPlan {
    std::map<ir::Inst*, NarrowLoadAction> narrow_loads;
    std::map<ir::Inst*, MemoryWritebackAction> writebacks;
    // EA Add/Sub folded into the access as an unscaled simm9 offset
    // (LDAPUR/STLUR); the EA instruction is in `covered`.
    std::map<ir::Inst*, MemoryOffsetAction> offsets;
    std::vector<ir::Inst*> address_aliases, covered;
};
MemoryLoweringPlan BuildMemoryLoweringPlan(ir::Block* block, const RegAlloc& allocation,
        const FeatureSet& features, const GPRBlockPlan& gpr, const GPRResidenceFacts& residences,
        MemoryLoweringOptions options, const BlockAnalysisIndex* analysis = nullptr);
}
