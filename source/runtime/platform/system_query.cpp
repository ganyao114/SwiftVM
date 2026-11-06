// Host service implementation of the architectural query callback. Numerical
// guest CPU/x87/vector helpers do not acquire entropy or read a host clock.
#include "runtime/arch/x86/cpu.h"
#include <atomic>
#include <chrono>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/random.h>
#endif

namespace swift::runtime::x86 {
CpuQueryResult QueryGuestSystem(std::uint64_t kind, std::uint64_t) noexcept {
    if (kind == 2 || kind == 3) {
        std::uint64_t value{};
#if defined(__linux__) || defined(__APPLE__)
        const bool success=::getentropy(&value,sizeof value)==0;
#else
        const bool success=false;
#endif
        return {success?value:0,success?1u:0u};
    }
    if (kind == 1) std::atomic_thread_fence(std::memory_order_seq_cst);
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    return {static_cast<std::uint64_t>(ticks), 0};
}
} // namespace swift::runtime::x86
