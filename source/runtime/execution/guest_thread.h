#pragma once

#include <cstdint>
#include "base/error.h"

#include "dispatch.h"
#include "fault.h"
#include "runtime/arch/x86/state.h"

namespace swift::runtime::execution {
// The VM currently owns exactly one guest thread by value. Live host state
// and recovery borrows never become shared code-cache ownership.
struct GuestThread {
    struct ArmReservation {
        std::uint64_t address{};
        std::uint32_t bytes{};
        AtomicValue observed;
    } arm_reservation;

    FaultContext faults;
    NativeRunContext native;
    DispatchCache l1;
    backend::NativeCode anchor;
    std::uint8_t fp_mode{};
    swift::Failure bridge_failure;
    std::uint64_t clear_child_tid{};
    bool exited{}, interpret_requested{};
    int exit_code{};
    GuestStopReason stop_reason{GuestStopReason::GuestExit};
    int termination_signal{};
};
}  // namespace swift::runtime::execution
