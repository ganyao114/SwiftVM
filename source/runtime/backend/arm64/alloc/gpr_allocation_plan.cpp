#include "runtime/backend/reg_alloc.h"
#include "target_allocation_planner.h"

namespace swift::runtime::backend {
void RegAlloc::CompleteGPRAllocation(const std::shared_ptr<Module>& module) {
    if (target_complete || target_planning || (!allocation_block && !allocation_function)) return;
    arm64::CompleteARM64TargetPlanning(module, *this, allocation_block,
                                     allocation_function, static_cast<bool>(allocation_replay));
}
} // namespace swift::runtime::backend
