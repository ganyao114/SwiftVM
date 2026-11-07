#pragma once

#include <cstdint>
#include <string>

#include "dispatch.h"
#include "fault.h"
#include "runtime/arch/x86/state.h"

namespace swift::runtime::execution {
// The VM currently owns exactly one guest thread by value. Live host state
// and recovery borrows never become shared code-cache ownership.
struct GuestThread {
    FaultContext faults;
    NativeRunContext native;
    backend::NativeCode anchor;
    std::uint8_t fp_mode{};
    std::string diagnostic;
    const char* resolver_error{};
    const char* access_error{};
    std::uint64_t clear_child_tid{};
    bool exited{}, interpret_requested{};
    int exit_code{};
};
}  // namespace swift::runtime::execution
