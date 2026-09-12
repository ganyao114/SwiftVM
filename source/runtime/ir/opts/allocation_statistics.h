#pragma once
#include "base/types.h"
namespace swift::runtime::ir {
struct AllocationStatistics {
    u32 eviction_restarts{};
    u32 final_gpr_reserve{};
    u32 final_fpr_reserve{};
    bool fell_back_to_ladder{};
};
}
