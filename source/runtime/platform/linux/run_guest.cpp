#include "aot/artifact.h"
#include "host_runtime.h"
#include "runtime/execution/timing.h"
#include "runtime/execution/vm.h"

#include <array>
#include <chrono>
#include <exception>
#include "base/error.h"
#include "base/format.h"

namespace swift::runtime::execution {
RunResult RunGuest(const std::string& executable,
                   std::span<const std::string> argv,
                   std::span<const std::string> environment,
                   RuntimeOptions options) {
    using Clock = std::chrono::steady_clock;
    RuntimeStats stats;
    int code = 125;
    std::string error;
    swift::Failure recorded;
    swift::Failure cleanup_failure;
    GuestStopReason stopped{GuestStopReason::GuestExit};
    int termination_signal{};
    std::unique_ptr<GuestExceptionSnapshot> snapshot;
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
            if (options.artifact_path.empty())
                code = vm.Run(executable, argv, environment);
            else {
                aot::Artifact artifact;
                {
                    RuntimeTimer total{stats.compile_request_ns}, load{stats.cache_load_ns};
                    std::string reason;
                    if (!aot::ReadArtifact(options.artifact_path, artifact, reason))
                        swift::ThrowError(std::move(reason), ErrorKind::InvalidArgument);
                }
                const std::array default_arguments{artifact.guest_path};
                const auto guest_arguments =
                        argv.empty() ? std::span<const std::string>{default_arguments} : argv;
                vm.PrepareGuest(artifact.guest_elf,
                                artifact.guest_path,
                                guest_arguments,
                                environment,
                                &artifact.code);
                code = vm.RunPrepared();
            }
        } catch (const std::exception& failure) {
            recorded.Capture(failure);
            error = failure.what();
            stopped = GuestStopReason::HostFailure;
        }
        if (vm.StopReason() != GuestStopReason::GuestExit) {
            stopped = vm.StopReason();
            termination_signal = vm.TerminationSignal();
            snapshot = vm.CaptureException();
            if (error.empty())
                error = swift::Format("guest event signal={} pc={:#x}",
                                      termination_signal,
                                      snapshot ? snapshot->info.instruction_pc : 0);
        }
        const auto released = vm.Close();
        if (!released.Complete()) {
            if (!error.empty()) error += "; ";
            error += swift::Format(
                    "native mapping release failed: remaining={} unmap_errno={} close_errno={}",
                    released.remaining_views,
                    released.unmap_error,
                    released.close_error);
            cleanup_failure.Set(ErrorKind::System,
                                "native mapping release failed",
                                released.unmap_error ? released.unmap_error : released.close_error);
        }
    } catch (const std::exception& failure) {
        recorded.Capture(failure);
        error = failure.what();
        stopped = GuestStopReason::HostFailure;
    }
    stats.run_lifetime_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    if (options.decode_timing)
        stats.compile_without_byte_decode_ns =
                stats.compile_request_ns > stats.compile_decode.nanoseconds
                        ? stats.compile_request_ns - stats.compile_decode.nanoseconds
                        : 0;
    return {code,
            stats,
            std::move(error),
            recorded,
            stopped,
            termination_signal,
            std::move(snapshot),
            cleanup_failure};
}
#if !defined(__aarch64__) && !(defined(__riscv) && __riscv_xlen == 64) && !defined(__x86_64__) &&  \
        !defined(__loongarch64)
extern "C" std::uint64_t SvmRunNative(NativeRunContext*) noexcept {
    return ExitValue(NativeExit::ResolveFailure);
}

extern "C" void SvmNativeDispatch() noexcept {}

extern "C" void SvmNativeDispatchCounted() noexcept {}

extern "C" void SvmNativeDispatchNZCV() noexcept {}

extern "C" void SvmNativeDispatchNZCVCounted() noexcept {}

extern "C" void SvmNativePending() noexcept {}

extern "C" void SvmNativePendingCounted() noexcept {}

extern "C" void SvmNativeExit() noexcept {}

extern "C" void SvmNativeExceptionExit() noexcept {}
#endif
}  // namespace swift::runtime::execution
