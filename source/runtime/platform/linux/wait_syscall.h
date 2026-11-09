#pragma once
#include <cstdint>

namespace swift::runtime::platform {
struct HostWaitRequest {
    std::uint64_t number{}, arguments[6]{}, mask{};
};

static_assert(sizeof(HostWaitRequest) == 64);
inline constexpr std::int64_t CancelledBeforeCall = -4096;
extern "C" std::int64_t SvmHostWaitSyscall(const HostWaitRequest*) noexcept;
extern "C" const char SvmHostWaitBegin[], SvmHostWaitComplete[], SvmHostWaitCancelled[];
bool CancelHostWait(void* raw) noexcept;
}  // namespace swift::runtime::platform
