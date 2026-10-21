#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_owner_slot.h"
#include "runtime/backend/smc_tracker.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using Status = RetirementControl::DrainStatus;
unsigned checks{};

void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

Config TestConfig() {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x4000;
    config.enable_jit = true;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
#if defined(__riscv) && __riscv_xlen == 64
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    return config;
}

void Control() {
    auto control = std::make_shared<RetirementControl>();
    u64 request{};
    auto owner = std::make_unique<RetirementControl::ExitRegistration>(control, request);
    auto ticket = RetirementControl::RequestDrain(control);
    Check(control->CheckDrain(ticket) == Status::Ready, "idle owner does not block range drain");
    Check(control->CheckShutdown(ticket) == Status::Stale, "ordinary ticket does not authorize destruction");
    Check(control->SealPublications(ticket) && control->Resume(ticket), "ordinary drain still reopens");
    {
        RetirementControl::Lease producer{control, RetirementControl::Kind::Producer};
        ticket = RetirementControl::RequestDrain(control);
        Check(control->CheckShutdown(ticket) == Status::Stale &&
              control->WaitShutdown(ticket) == Status::Stale,
              "active ordinary ticket also refuses terminal waiting");
    }
    Check(control->SealPublications(ticket) && control->Resume(ticket), "ordinary active drain reopens");
    {
        RetirementControl::Lease producer{control, RetirementControl::Kind::Producer};
        ticket = RetirementControl::RequestDrain(control, true);
        Check(control->CheckShutdown(ticket) == Status::SelfActive &&
              control->WaitShutdown(ticket) == Status::SelfActive,
              "terminal caller never waits for its own producer lease");
        Check(!control->SealPublications(ticket), "self-active producer cannot seal");
    }
    Check(control->CheckDrain(ticket) == Status::Ready &&
          control->CheckShutdown(ticket) == Status::Pending, "ordinary ready is insufficient for shutdown");
    Check(control->SealPublications(ticket), "seal terminal publications after producer exit");
    Check(control->CheckShutdown(ticket) == Status::Pending, "registered idle owner remains attached");
    bool rejected{};
    u64 late_request{};
    try { RetirementControl::ExitRegistration late{control, late_request}; }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected && late_request == 0, "sealed terminal state rejects late owner before linking");
    std::atomic_bool ready{};
    std::thread waiter{[&] {
        ready.store(control->WaitShutdown(ticket) == Status::Ready);
    }};
    while (control->ShutdownWaiters() == 0) std::this_thread::yield();
    const bool still_attached = !ready.load();
    owner.reset();
    waiter.join();
    Check(still_attached && ready.load(), "final owner unlink wakes terminal waiter");
    Check(control->CheckShutdown(ticket) == Status::Ready && !control->Resume(ticket),
          "terminal readiness cannot reopen admission");
    Check(control->ShutdownWaiters() == 0, "completed terminal waiter leaves diagnostics");

    auto unsealed = std::make_shared<RetirementControl>();
    const auto unsealed_ticket = RetirementControl::RequestDrain(unsealed, true);
    Check(unsealed->CheckShutdown(unsealed_ticket) == Status::Pending,
          "terminal close without publication seal is not ready");
    std::atomic_bool seal_woke{};
    std::thread seal_waiter{[&] {
        seal_woke.store(unsealed->WaitShutdown(unsealed_ticket) == Status::Ready);
    }};
    while (unsealed->ShutdownWaiters() == 0) std::this_thread::yield();
    const bool still_unsealed = !seal_woke.load();
    const bool seal_succeeded = unsealed->SealPublications(unsealed_ticket);
    seal_waiter.join();
    Check(still_unsealed && seal_succeeded && seal_woke.load() && unsealed->ShutdownWaiters() == 0,
          "seal notification wakes a waiter already inside terminal waiting");

    auto execution_control = std::make_shared<RetirementControl>();
    {
        RetirementControl::Lease execution{execution_control, RetirementControl::Kind::Execution};
        const auto self = RetirementControl::RequestDrain(execution_control, true);
        Check(execution_control->WaitShutdown(self) == Status::SelfActive,
              "execution callback cannot wait for its own frame");
    }
}

void RuntimeOwners(bool creator_exits) {
    AddressSpace space{TestConfig()};
    auto control = space.GetRetirementControl();
    std::unique_ptr<Runtime> first, second;
    std::atomic_bool constructed{}, leave{};
    std::thread creator{[&] {
        first = std::make_unique<Runtime>(&space);
        second = std::make_unique<Runtime>(&space);
        constructed.store(true, std::memory_order_release);
        if (!creator_exits) {
            while (!leave.load(std::memory_order_acquire)) std::this_thread::yield();
        }
    }};
    while (!constructed.load(std::memory_order_acquire)) std::this_thread::yield();
    if (creator_exits) creator.join();
    const auto ticket = RetirementControl::RequestDrain(control, true);
    const bool idle_ready = control->CheckDrain(ticket) == Status::Ready;
    const bool sealed = control->SealPublications(ticket);
    const bool two_attached = control->CheckShutdown(ticket) == Status::Pending;
    first.reset();
    const bool one_attached = control->CheckShutdown(ticket) == Status::Pending;
    second.reset();
    const bool none_attached = control->WaitShutdown(ticket) == Status::Ready;
    leave.store(true, std::memory_order_release);
    if (!creator_exits) creator.join();
    Check(idle_ready && sealed, "real idle runtimes allow ordinary drain and terminal seal");
    Check(two_attached && one_attached, "every real Runtime attachment remains independently counted");
    Check(none_attached, "cross-thread Runtime destruction finishes before terminal ready");
    bool rejected{};
    try { Runtime late{&space}; }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "terminal gate rejects fresh real Runtime construction");
}

struct Payload { u64 value{0x1122334455667788}; };

void Slots() {
    SignalOwnerSlot slot;
    Check(!slot.Publish(nullptr), "null owner is never published");
    for (unsigned round = 0; round < 64; ++round) {
        Payload payload;
        Check(slot.Publish(&payload) && !slot.Publish(&payload), "only one owner occupies a slot");
        std::atomic_bool entered{}, leave{}, detached{}, valid{};
        std::thread reader{[&] {
            SignalOwnerSlot::Reader held{&slot};
            SignalOwnerSlot::Reader nested{&slot};
            entered.store(true);
            while (!leave.load()) std::this_thread::yield();
            auto* observed = static_cast<Payload*>(held.Get());
            valid.store(observed == &payload && nested.Get() == observed &&
                        observed->value == 0x1122334455667788);
        }};
        while (!entered.load()) std::this_thread::yield();
        std::thread destroyer{[&] {
            slot.DetachAndWait(&payload);
            payload.value = 0;
            detached.store(true);
        }};
        while (slot.IsPublished()) std::this_thread::yield();
        const bool waiting = !detached.load();
        bool late_null{};
        {
            SignalOwnerSlot::Reader late{&slot};
            late_null = late.Get() == nullptr;
        }
        leave.store(true);
        reader.join();
        destroyer.join();
        Check(waiting && late_null, "revoke prevents new reads while prior owner reader blocks destruction");
        Check(valid.load() && detached.load() && payload.value == 0,
              "loaded owner remains alive through its last reader");
    }
}

SignalOwnerSlot* signal_slot{};
std::atomic_bool signal_entered{}, signal_leave{}, signal_valid{};

void HoldSignalReader(int) {
    SignalOwnerSlot::Reader held{signal_slot};
    SignalOwnerSlot::Reader nested{signal_slot};
    auto* payload = static_cast<Payload*>(held.Get());
    signal_entered.store(true, std::memory_order_seq_cst);
    while (!signal_leave.load(std::memory_order_seq_cst)) {}
    signal_valid.store(payload && nested.Get() == payload &&
                       payload->value == 0x1122334455667788, std::memory_order_seq_cst);
}

void Signals() {
    static_assert(std::atomic_bool::is_always_lock_free);
    struct sigaction action{}, previous{};
    action.sa_handler = &HoldSignalReader;
    sigemptyset(&action.sa_mask);
    Check(sigaction(SIGUSR2, &action, &previous) == 0, "install real signal reader");
    const auto restore = ScopeExit([&] { sigaction(SIGUSR2, &previous, nullptr); });
    SignalOwnerSlot slot;
    Payload payload;
    signal_slot = &slot;
    Check(slot.Publish(&payload), "publish actual signal payload");
    std::thread owner_thread{[] { raise(SIGUSR2); }};
    while (!signal_entered.load(std::memory_order_seq_cst)) std::this_thread::yield();
    std::atomic_bool detached{};
    std::thread destroyer{[&] {
        slot.DetachAndWait(&payload);
        payload.value = 0;
        detached.store(true);
    }};
    while (slot.IsPublished()) std::this_thread::yield();
    const bool waiting_for_signal = !detached.load();
    signal_leave.store(true, std::memory_order_seq_cst);
    owner_thread.join();
    destroyer.join();
    signal_slot = nullptr;
    Check(waiting_for_signal && signal_valid.load() && detached.load(),
          "actual signal reader pins payload until interrupted handler returns");
}

void Destruction() {
    const auto child = fork();
    Check(child >= 0, "fork live-Runtime teardown control");
    if (child == 0) {
        std::set_terminate([] { _exit(86); });
        auto space = std::make_unique<AddressSpace>(TestConfig());
        auto runtime = std::make_unique<Runtime>(space.get());
        space.reset();
        // Deliberately avoid dereferencing the Runtime if the guard is missing.
        _exit(87);
    }
    int status{};
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
          "AddressSpace refuses teardown with an idle borrowed Runtime");
    {
        AddressSpace space{TestConfig()};
        { Runtime runtime{&space}; }
    }
    Check(true, "ordinary Runtime-before-AddressSpace destruction still succeeds");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        SmcTracker::SetEnabled(false);
        if (std::strcmp(argv[1], "control") == 0) Control();
        else if (std::strcmp(argv[1], "runtime") == 0) { RuntimeOwners(false); RuntimeOwners(true); }
        else if (std::strcmp(argv[1], "slots") == 0) Slots();
        else if (std::strcmp(argv[1], "signals") == 0) Signals();
        else if (std::strcmp(argv[1], "destruction") == 0) Destruction();
        else return 2;
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "shutdown owner failure: %s\n", error.what());
        return 1;
    }
}
