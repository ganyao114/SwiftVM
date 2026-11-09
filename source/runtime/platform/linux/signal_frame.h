#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "runtime/exception.h"

namespace swift::runtime::execution {
class GuestMemory;
}

namespace swift::runtime::platform {
struct GuestSignalAction {
    std::uint64_t handler{}, flags{}, restorer{}, mask{};
};

struct GuestSignalStack {
    std::uint64_t address{};
    std::uint32_t flags{2}, padding{};
    std::uint64_t size{};
};

struct GuestSignalInfo {
    std::int32_t number{}, error{}, code{}, padding{};
    std::array<std::uint8_t, 112> payload{};
};

static_assert(sizeof(GuestSignalAction) == 32 && sizeof(GuestSignalStack) == 24 &&
              sizeof(GuestSignalInfo) == 128);

struct SignalFrameContext {
    std::uint64_t mask{};
    GuestSignalStack stack;
};

// A temporary encoding, owned only until GuestWrite commits the complete frame.
// It never contains a host continuation or a borrowed code-cache pointer.
struct EncodedSignalFrame {
    std::uint64_t address{};
    std::vector<std::uint8_t> bytes;
};

bool EncodeX86SignalFrame(const x86::GuestState&,
                          const GuestExceptionInfo&,
                          const GuestSignalInfo&,
                          const GuestSignalAction&,
                          const SignalFrameContext&,
                          std::uint64_t restorer,
                          EncodedSignalFrame&,
                          x86::GuestState& handler);
bool DecodeX86SignalFrame(const execution::GuestMemory&,
                          const x86::GuestState&,
                          x86::GuestState& restored,
                          SignalFrameContext&);
bool EncodeArm64SignalFrame(const arm64::GuestState&,
                            const GuestExceptionInfo&,
                            const GuestSignalInfo&,
                            const GuestSignalAction&,
                            const SignalFrameContext&,
                            std::uint64_t restorer,
                            EncodedSignalFrame&,
                            arm64::GuestState& handler);
bool DecodeArm64SignalFrame(const execution::GuestMemory&,
                            const arm64::GuestState&,
                            arm64::GuestState& restored,
                            SignalFrameContext&);

inline bool OnSignalStack(std::uint64_t sp, const GuestSignalStack& stack) noexcept {
    return !(stack.flags & 2) && sp >= stack.address && sp - stack.address < stack.size;
}
}  // namespace swift::runtime::platform
