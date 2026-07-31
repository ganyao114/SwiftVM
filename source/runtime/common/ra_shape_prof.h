// Read-only register-allocation shape probe (W67).
//
// The probe is translation-only.  No counter is emitted into generated code,
// and every call site is guarded by the process-constant
// RAShapeProfEnabled().  Aggregation uses relaxed atomics so concurrently
// compiling guest threads never race on ordinary integers.
#pragma once

#include <array>
#include <cstddef>

#include "runtime/common/types.h"

namespace swift::runtime::ir {
class Block;
}

namespace swift::runtime {

enum class RAShapeHelperABI : u8 {
    DirectAAPCS,
    IndirectAAPCS,
    XStateSyncAAPCS,
    Count,
};

struct RAShapeHelperCounters {
    u64 calls{};
    u64 snapshot_instructions{};
    u64 snapshot_code_bytes{};
    u64 snapshot_memory_bytes{};
    u64 xmm_sync_instructions{};
    u64 xmm_sync_memory_bytes{};
};

// One compilation unit's local accumulator.  RegAlloc owns it; JitContext
// adds emission facts and submits it once FinalizeCode succeeds.
struct RAShapeUnitCounters {
    bool ra_valid{};
    u32 gpr_pool{};
    u32 fpr_pool{};
    u32 max_live_gpr{};
    u32 max_live_fpr{};
    u32 scratch_gpr{};
    u32 scratch_fpr{};
    u32 spill_defs{};
    u32 spill_high_water{};
    u64 spill_loads{};
    u64 spill_stores{};
    u64 consecutive_pair_fallbacks{};

    std::array<RAShapeHelperCounters,
               static_cast<std::size_t>(RAShapeHelperABI::Count)>
            helpers{};

    u64 pf_producers{};
    u64 af_producers{};
    u64 pf_direct_consumers{};
    u64 af_direct_consumers{};
    u64 pf_materialize{};
    u64 af_materialize{};
    u64 pf_force_edge{};
    u64 af_force_edge{};
    u64 pf_force_helper{};
    u64 af_force_helper{};
    u64 pf_force_fault{};
    u64 af_force_fault{};
    u64 pf_force_other{};
    u64 af_force_other{};
};

[[nodiscard]] bool RAShapeProfEnabled();
void RAShapeAnalyzeFlags(const ir::Block* block, RAShapeUnitCounters& unit);
void RAShapeRecordHelperTarget(VAddr target,
                               RAShapeHelperABI abi,
                               const RAShapeHelperCounters& counters);
void RAShapeSubmitUnit(const RAShapeUnitCounters& unit);

}  // namespace swift::runtime
