#include "host_runtime.h"
#include "runtime/execution/vm.h"

#include <chrono>
#include <exception>

namespace swift::runtime::execution {
RunResult RunGuest(const std::string& executable,
                   std::span<const std::string> argv,
                   std::span<const std::string> environment,
                   RuntimeOptions options) {
    using Clock = std::chrono::steady_clock;
    RuntimeStats stats;
    int code = 125;
    std::string error;
    const auto begin = Clock::now();
    try {
        const auto binding_begin = Clock::now();
        platform::HostRuntime host{options.decode_timing};
        const auto backend = host.Bind();
        const auto binding_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - binding_begin)
                        .count());
        stats.compile_request_ns += binding_ns;
        stats.fault_service_ns += binding_ns;
        VM vm{options, stats, backend};
        try {
            code = vm.Run(executable, argv, environment);
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        const auto released = vm.Close();
        if (!released.Complete()) {
            if (!error.empty()) error += "; ";
            error += "native mapping release failed: remaining=" +
                     std::to_string(released.remaining_views) +
                     " unmap_errno=" + std::to_string(released.unmap_error) +
                     " close_errno=" + std::to_string(released.close_error);
        }
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    stats.run_lifetime_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    if (options.decode_timing)
        stats.compile_without_byte_decode_ns =
                stats.compile_request_ns > stats.compile_decode.nanoseconds
                        ? stats.compile_request_ns - stats.compile_decode.nanoseconds
                        : 0;
    return {code, stats, std::move(error)};
}
#if !defined(__aarch64__) && !(defined(__riscv) && __riscv_xlen == 64)
extern "C" std::uint64_t SvmRunNative(NativeRunContext*) noexcept { return 2; }

extern "C" void SvmNativeDispatch() noexcept {}

extern "C" void SvmNativeDispatchCounted() noexcept {}

extern "C" void SvmNativeDispatchNZCV() noexcept {}

extern "C" void SvmNativeDispatchNZCVCounted() noexcept {}

extern "C" void SvmNativePending() noexcept {}

extern "C" void SvmNativePendingCounted() noexcept {}

extern "C" void SvmNativeExit() noexcept {}
#endif
}  // namespace swift::runtime::execution
