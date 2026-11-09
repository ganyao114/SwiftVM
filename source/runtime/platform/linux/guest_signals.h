#pragma once

#include <array>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <vector>
#include "signal_frame.h"

namespace swift::runtime::execution {
class VM;
}

namespace swift::runtime::platform {
enum class SignalDelivery : std::uint8_t { Resume, Terminate, Unhandled };

// The VM's single guest thread owns dispositions, masks and pending records.
// Handler ingress and ordinary owner operations serialize by blocking ingress;
// no foreign thread may write these FIFOs.
class GuestSignals {
public:
    explicit GuestSignals(execution::VM&) noexcept;
    ~GuestSignals();
    SignalDelivery Deliver(const GuestExceptionInfo&);
    bool Pending() const noexcept;
    bool TakePending(GuestExceptionInfo&) noexcept;
    std::int64_t Action(int, std::uint64_t action, std::uint64_t old, std::uint64_t size);
    std::int64_t Mask(int how, std::uint64_t set, std::uint64_t old, std::uint64_t size);
    std::int64_t AltStack(std::uint64_t set, std::uint64_t old);
    bool ReturnFrame(bool realtime = true);
    void InterruptSyscall(std::uint64_t number,
                          const std::uint64_t* arguments,
                          bool restartable,
                          bool restart_without_handler) noexcept;

    bool HasInterruptedSyscall() const noexcept { return interrupted.active; }

    void CompleteInterruptedWithoutHandler() noexcept;
    bool ConsumeRestartDeadline(std::uint64_t number,
                                const std::uint64_t* arguments,
                                timespec&) noexcept;
    void PreserveFutexDeadline(const timespec&) noexcept;
    std::int64_t PendingMask(std::uint64_t output, std::uint64_t size);
    std::int64_t Suspend(std::uint64_t input, std::uint64_t size);
    std::int64_t WaitInfo(std::uint64_t input,
                          std::uint64_t info,
                          std::uint64_t timeout,
                          std::uint64_t size,
                          bool time64 = false);
    std::int64_t Queue(int, const GuestSignalInfo&);
    std::int64_t WaitSyscall(std::uint64_t number, const std::uint64_t* args);
    static bool HostIngress(void*, int, siginfo_t*, void*) noexcept;
    static constexpr int WakeSignal = 23;  // Private SI_QUEUE payload distinguishes notification.

private:
    struct RealtimeQueue {
        std::vector<GuestSignalInfo> records;
        std::uint32_t first{}, count{};
    };

    execution::VM& vm;
    std::array<GuestSignalAction, 65> actions{};
    std::array<GuestSignalInfo, 32> standard{};
    std::array<RealtimeQueue, 33> realtime;
    std::uint64_t blocked{}, pending{}, owned{}, pressure{}, waited{};
    GuestSignalStack stack;
    GuestSignalInfo selected;
    int selected_number{};
    std::uint64_t restorer{};
    bool wait_active{};

    struct InterruptedSyscall {
        std::uint64_t instruction_pc{}, resume_pc{}, number{}, arguments[6]{};
        timespec deadline{};
        bool active{}, restartable{}, restart_without_handler{}, deadline_valid{}, restart_ready{};
    } interrupted;

    bool restore_suspend_mask{};
    std::uint64_t suspend_mask{};
    void Ready() noexcept;
    bool Enqueue(int, const GuestSignalInfo&) noexcept;
    bool OwnSignals(std::uint64_t);
    std::uint64_t Restorer(bool realtime = true);
    GuestSignalStack StackView() const noexcept;
    void Terminate(int) noexcept;
    void RestoreInterruptedCall() noexcept;
    bool MatchesRestartContext() const noexcept;
    bool Ingress(int, siginfo_t*, void*) noexcept;
};
}  // namespace swift::runtime::platform
