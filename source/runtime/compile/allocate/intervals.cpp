#include "runtime/common/perf_stats.h"
#include "runtime/compile/allocate/internal.h"

namespace swift::runtime::compile::detail {

bool LinearScan::ValueUsesStayInBlock(const Block* block, Value value) const {
    return value_liveness.UsesStayInBlock(block, value);
}

void LinearScan::CollectLiveIntervalsSingleBlock(const HIRFunction* function) {
    ASSERT_MSG(function->GetHIRBlocksRPO().size() == 1,
               "single-block register allocator received {} blocks",
               function->GetHIRBlocksRPO().size());
    CollectLiveIntervals(function);
}

void LinearScan::CollectLiveIntervals(const HIRFunction* function) {
    fixed_fpr_read_ends.clear();
    fixed_read_cursor = 0;
    PerfScope2 perf_values{GetPerfStats2().regalloc_live_values};
    // HIR values are already in final RPO-id order. Base use ends, host writes
    // and locality belong to the immutable input and survive every retry.
    for (const auto* value : function->GetHIRValues()) {
        if (!value) continue;
        const auto* inst = value->value.Def();
        const u32 id = value->GetOrderId();
        if (MapPreallocatedGPR(id)) continue;
        const u32 current_end = std::max(id, value_liveness.UseEnd(inst->Id()));
        const u32 end = value_liveness.FunctionEnd(inst->Id());
        const u32 owner_end =
                single_block_fast_path ? UINT32_MAX : value_liveness.DefinitionBlockEnd(inst);
        if (MapFixedInput(id, end, current_end, owner_end)) continue;
        if (inst->IsBitCastOperation()) {
            reg_alloc->MapReference(inst->GetArg<Value>(0).Id(), inst->Id());
            continue;
        }
        const u32 start = inst->IsPseudoOperation() ? inst->GetArg<Value>(0).Id() : id;
        live_interval.push_back({inst, start, end});
    }
}

void LinearScan::CollectLiveIntervals(const Block* block) {
    ASSERT_MSG(block && !block->IsEmptyBlock(), "block is empty");
    fixed_fpr_read_ends.clear();
    fixed_read_cursor = 0;
    PerfScope2 perf_values{GetPerfStats2().regalloc_live_values};
    for (const auto& inst : block->GetInstList()) {
        if (inst.HasValue() && MapPreallocatedGPR(inst.Id())) continue;
        const u32 end = value_liveness.UseEnd(inst.Id());
        if (MapFixedInput(inst.Id(), end, std::max<u32>(inst.Id(), end), UINT32_MAX)) continue;
        if (inst.IsBitCastOperation()) {
            reg_alloc->MapReference(inst.GetArg<Value>(0).Id(), inst.Id());
            continue;
        }
        // A surviving side-effecting definition still needs a destination
        // even when no operand consumes its result.
        if (inst.HasValue()) live_interval.push_back({&inst, inst.Id(), end ? end : inst.Id()});
    }
}

}  // namespace swift::runtime::compile::detail
