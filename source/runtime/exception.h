#pragma once

#include <cstdint>
#include <variant>

#include "runtime/arch/arm64/state.h"
#include "runtime/arch/guest.h"
#include "runtime/arch/x86/state.h"
#include "runtime/exit.h"

namespace swift::runtime {

enum class GuestExceptionOrigin : std::uint8_t { Native, Semantic, Async };
enum class GuestExceptionAccess : std::uint8_t {
    Unknown = 0,
    Read = 1,
    Write = 2,
    Atomic = 3,
    Execute = 4,
};

struct GuestExceptionInfo {
    std::uint64_t instruction_pc{}, address{}, syndrome{}, host_pc{};
    std::int32_t signal_number{}, signal_code{};
    std::uint32_t unit_id{UINT32_MAX}, native_offset{UINT32_MAX};
    GuestISA isa{GuestISA::X86_64};
    GuestExceptionKind kind{GuestExceptionKind::Memory};
    GuestExceptionOrigin origin{GuestExceptionOrigin::Native};
    GuestExceptionAccess access{GuestExceptionAccess::Unknown};
    std::uint8_t access_bytes{};
    bool address_valid{}, syndrome_valid{};
    std::uint16_t exception_vector{UINT16_MAX};
};

static_assert(sizeof(GuestExceptionInfo) == 64);

enum class GuestStopReason : std::uint8_t {
    GuestExit,
    GuestSignalTermination,
    UnhandledGuestEvent,
    HostFailure,
};

using GuestContextView = std::variant<x86::GuestState*, arm64::GuestState*>;
enum class GuestExceptionDisposition : std::uint8_t { GuestOS, Resume, Stop };
using GuestExceptionHandler = GuestExceptionDisposition (*)(void*,
                                                            GuestContextView,
                                                            const GuestExceptionInfo&);

// Allocated only when a stopped guest's context must outlive its VM. Internal
// packed flags remain typed state; ABI exporters use the ISA conversion helpers.
struct GuestExceptionSnapshot {
    GuestExceptionInfo info;
    std::variant<x86::GuestState, arm64::GuestState> state;
};

}  // namespace swift::runtime
