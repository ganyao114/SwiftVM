#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include "runtime/common/types.h"

namespace swift::runtime::backend {

class InterruptPollState;
class TerminalAliasOwner {
public:
    virtual ~TerminalAliasOwner() = default;
    virtual void Revoke() noexcept = 0;
};
// Host-only admission survives borrowed Runtime/Module handles. Draining
// leaves routing unchanged until existing producers finish and are sealed.
class RetirementControl {
public:
    enum class Kind { Execution, Producer };
    enum class DrainStatus { Ready, Pending, SelfActive, Stale };

    struct Ticket {
        std::shared_ptr<RetirementControl> owner;
        u64 generation{};
    };

    class PublicationScope;

    class ExitRegistration {
    public:
        ExitRegistration(const std::shared_ptr<RetirementControl>& owner, u64& request,
                         InterruptPollState* poll = nullptr);
        ~ExitRegistration();
        ExitRegistration(const ExitRegistration&) = delete;
        ExitRegistration& operator=(const ExitRegistration&) = delete;

    private:
        friend class RetirementControl;
        std::shared_ptr<RetirementControl> owner_;
        u64* request_;
        InterruptPollState* poll_;
        ExitRegistration* previous_{};
        ExitRegistration* next_{};
    };

    class Lease {
    public:
        Lease(const std::shared_ptr<RetirementControl>& owner, Kind kind);
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        explicit operator bool() const { return admitted_; }

        [[nodiscard]] u64 Generation() const { return admitted_ ? generation_ : 0; }

    private:
        friend class RetirementControl;
        friend class PublicationScope;
        static thread_local Lease* current_;
        std::shared_ptr<RetirementControl> owner_;
        Kind kind_;
        u64 generation_{};
        Lease* previous_{};
        bool admitted_{};
        bool counted_{};
    };

    class PublicationScope {
    public:
        enum class Mode { ProducerCompletion, OpenOnly };
        explicit PublicationScope(const Lease& lease, Mode mode = Mode::ProducerCompletion);
        PublicationScope(const PublicationScope&) = delete;
        PublicationScope& operator=(const PublicationScope&) = delete;
        explicit operator bool() const { return admitted_; }

    private:
        std::unique_lock<std::recursive_mutex> lock_;
        bool admitted_{};
    };

    [[nodiscard]] static Ticket RequestDrain(
            const std::shared_ptr<RetirementControl>& owner, bool terminal = false);
    [[nodiscard]] DrainStatus CheckDrain(const Ticket& ticket, bool producers_only = false) const;
    [[nodiscard]] DrainStatus WaitDrain(const Ticket& ticket, bool producers_only = false) const;
    // Terminal destruction additionally requires sealed publications and no
    // registered Runtime owners, including idle host-side signal recovery.
    [[nodiscard]] DrainStatus CheckShutdown(const Ticket& ticket) const;
    [[nodiscard]] DrainStatus WaitShutdown(const Ticket& ticket) const;
    [[nodiscard]] u64 ShutdownWaiters() const {
        return shutdown_waiters_.load(std::memory_order_seq_cst);
    }
    [[nodiscard]] bool SealPublications(const Ticket& ticket);
    // Caller must have finished the retirement transaction before reopening.
    [[nodiscard]] bool Resume(const Ticket& ticket);
    [[nodiscard]] bool AuthorizeInstructionResume(const Lease& execution, u64 interrupted_generation) const;
    [[nodiscard]] bool IsClosed() const { return generation_.load(std::memory_order_seq_cst) & 1; }

private:
    [[nodiscard]] bool HasThreadLease() const;
    [[nodiscard]] bool CanPublish(const Lease& lease, PublicationScope::Mode mode) const;
    void NotifyWaiters();
    mutable std::recursive_mutex state_mutex_;
    mutable std::mutex wait_mutex_;
    mutable std::condition_variable wait_cv_;
    mutable std::atomic<u64> shutdown_waiters_{0};
    std::atomic<u64> generation_{2};
    u64 instruction_resume_from_{};
    std::atomic<u64> executions_{0};
    std::atomic<u64> producers_{0};
    std::atomic<u64> attached_owners_{0};
    std::atomic_bool publications_sealed_{false};
    std::atomic_bool terminal_{false};
    ExitRegistration* exit_registrations_{};
};

} // namespace swift::runtime::backend
