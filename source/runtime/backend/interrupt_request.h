#pragma once

#include <atomic>
#include "runtime/backend/context.h"
#include "runtime/common/backedge_control.h"

namespace swift::runtime::backend {

// Only the execution owner (including its synchronous recovery path) accesses
// halt_reason. A remote requester publishes the sticky atomic Signal bit.
// Low SMC generations are consumed at restartable dispatch boundaries, never
// by an instruction-internal stopping point after a committed side effect.
inline HaltReason ObserveHaltRequest(State& state) {
    if (state.halt_reason != HaltReason::None) return state.halt_reason;
    if (std::atomic_ref<u64>(state.exit_request).load(std::memory_order_acquire) & kBackedgeSignalRequest)
        state.halt_reason = HaltReason::Signal;
    return state.halt_reason;
}

} // namespace swift::runtime::backend
