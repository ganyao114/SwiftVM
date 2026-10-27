#include "runtime/backend/retirement_control.h"
#include "runtime/backend/interrupt_poll_state.h"
#include "runtime/common/backedge_control.h"

#include <exception>
#include <stdexcept>

namespace swift::runtime::backend {

thread_local RetirementControl::Lease* RetirementControl::Lease::current_{};

RetirementControl::ExitRegistration::ExitRegistration(
        const std::shared_ptr<RetirementControl>& owner, u64& request, InterruptPollState* poll)
        : owner_(owner), request_(&request), poll_(poll) {
    if (!owner_) return;
    std::lock_guard guard(owner_->state_mutex_);
    if (owner_->terminal_.load(std::memory_order_seq_cst) &&
        owner_->publications_sealed_.load(std::memory_order_seq_cst)) {
        throw std::runtime_error("terminal owner registration is sealed");
    }
    next_ = owner_->exit_registrations_;
    if (next_) next_->previous_ = this;
    owner_->exit_registrations_ = this;
    owner_->attached_owners_.fetch_add(1, std::memory_order_seq_cst);
    if (owner_->IsClosed()) {
        std::atomic_ref<u64>(*request_).fetch_or(kRetirementExitRequest, std::memory_order_release);
        if (poll_) poll_->Arm();
    }
}

RetirementControl::ExitRegistration::~ExitRegistration() {
    if (!owner_) return;
    {
        std::lock_guard guard(owner_->state_mutex_);
        if (previous_) previous_->next_ = next_;
        else owner_->exit_registrations_ = next_;
        if (next_) next_->previous_ = previous_;
        owner_->attached_owners_.fetch_sub(1, std::memory_order_seq_cst);
    }
    owner_->NotifyWaiters();
}

RetirementControl::Lease::Lease(const std::shared_ptr<RetirementControl>& owner, Kind kind)
        : owner_(owner), kind_(kind) {
    if (!owner_) return;
    if (kind == Kind::Producer) {
        for (auto* outer = current_; outer; outer = outer->previous_) {
            if (outer->owner_ == owner_ && outer->kind_ == kind) {
                generation_ = outer->generation_;
                admitted_ = true;
                break;
            }
        }
    }
    if (!admitted_) {
        generation_ = owner_->generation_.load(std::memory_order_seq_cst);
        if (generation_ & 1) return;
        auto& count = kind == Kind::Producer ? owner_->producers_ : owner_->executions_;
        count.fetch_add(1, std::memory_order_seq_cst);
        // A late entrant touches only the shared control until this recheck.
        if (owner_->generation_.load(std::memory_order_seq_cst) != generation_) {
            count.fetch_sub(1, std::memory_order_seq_cst);
            owner_->NotifyWaiters();
            return;
        }
        admitted_ = counted_ = true;
    }
    previous_ = current_;
    current_ = this;
}

RetirementControl::Lease::~Lease() {
    if (!admitted_) return;
    if (current_ != this) std::terminate();
    current_ = previous_;
    if (counted_) {
        auto& count = kind_ == Kind::Producer ? owner_->producers_ : owner_->executions_;
        count.fetch_sub(1, std::memory_order_seq_cst);
        if (owner_->IsClosed()) owner_->NotifyWaiters();
    }
}

RetirementControl::PublicationScope::PublicationScope(const Lease& lease, Mode mode) {
    if (!lease.admitted_ || lease.kind_ != Kind::Producer) return;
    bool belongs_to_thread = false;
    for (auto* entry = Lease::current_; entry; entry = entry->previous_)
        if (entry == &lease) belongs_to_thread = true;
    if (!belongs_to_thread) return;
    lock_ = std::unique_lock{lease.owner_->state_mutex_};
    admitted_ = lease.owner_->CanPublish(lease, mode);
}

bool RetirementControl::CanPublish(const Lease& lease, PublicationScope::Mode mode) const {
    const auto generation = generation_.load(std::memory_order_seq_cst);
    return !publications_sealed_ &&
           (generation == lease.generation_ ||
            (mode == PublicationScope::Mode::ProducerCompletion &&
             generation == lease.generation_ + 1));
}

RetirementControl::Ticket RetirementControl::RequestDrain(
        const std::shared_ptr<RetirementControl>& owner, bool terminal) {
    if (!owner) return {};
    std::lock_guard guard(owner->state_mutex_);
    if (terminal) owner->terminal_.store(true, std::memory_order_seq_cst);
    auto generation = owner->generation_.load(std::memory_order_seq_cst);
    if (!(generation & 1)) {
        ++generation;
        owner->generation_.store(generation, std::memory_order_seq_cst);
    }
    for (auto* entry = owner->exit_registrations_; entry; entry = entry->next_) {
        std::atomic_ref<u64>(*entry->request_)
                .fetch_or(kRetirementExitRequest, std::memory_order_release);
        // Static links use fault-only polls when the latch is enabled.
        // Publish the request before protecting the page, as Signal and SMC do.
        if (entry->poll_) entry->poll_->Arm();
    }
    return {owner, generation};
}

bool RetirementControl::HasThreadLease() const {
    for (auto* entry = Lease::current_; entry; entry = entry->previous_)
        if (entry->owner_.get() == this) return true;
    return false;
}

RetirementControl::DrainStatus RetirementControl::CheckDrain(
        const Ticket& ticket, bool producers_only) const {
    if (ticket.owner.get() != this || !(ticket.generation & 1) ||
        generation_.load(std::memory_order_seq_cst) != ticket.generation) return DrainStatus::Stale;
    if (HasThreadLease()) return DrainStatus::SelfActive;
    if (producers_.load(std::memory_order_seq_cst) != 0 ||
        (!producers_only && executions_.load(std::memory_order_seq_cst) != 0)) return DrainStatus::Pending;
    return DrainStatus::Ready;
}

RetirementControl::DrainStatus RetirementControl::WaitDrain(
        const Ticket& ticket, bool producers_only) const {
    std::unique_lock guard(wait_mutex_);
    DrainStatus status;
    wait_cv_.wait(guard, [&] {
        status = CheckDrain(ticket, producers_only);
        return status != DrainStatus::Pending;
    });
    return status;
}

RetirementControl::DrainStatus RetirementControl::CheckShutdown(const Ticket& ticket) const {
    if (!terminal_.load(std::memory_order_seq_cst)) return DrainStatus::Stale;
    const auto drained = CheckDrain(ticket);
    if (drained != DrainStatus::Ready) return drained;
    if (!publications_sealed_.load(std::memory_order_seq_cst) ||
        attached_owners_.load(std::memory_order_seq_cst) != 0) return DrainStatus::Pending;
    return DrainStatus::Ready;
}

RetirementControl::DrainStatus RetirementControl::WaitShutdown(const Ticket& ticket) const {
    std::unique_lock guard(wait_mutex_);
    struct Waiting {
        std::atomic<u64>& count;
        bool observed_pending{};
        void ObservePending() {
            if (!observed_pending) {
                count.fetch_add(1, std::memory_order_seq_cst);
                observed_pending = true;
            }
        }
        ~Waiting() {
            if (observed_pending) count.fetch_sub(1, std::memory_order_seq_cst);
        }
    } waiting{shutdown_waiters_};
    DrainStatus status;
    wait_cv_.wait(guard, [&] {
        status = CheckShutdown(ticket);
        if (status == DrainStatus::Pending) waiting.ObservePending();
        return status != DrainStatus::Pending;
    });
    return status;
}

void RetirementControl::NotifyWaiters() {
    std::lock_guard guard(wait_mutex_);
    wait_cv_.notify_all();
}

bool RetirementControl::SealPublications(const Ticket& ticket) {
    {
        std::lock_guard guard(state_mutex_);
        if (CheckDrain(ticket, true) != DrainStatus::Ready) return false;
        publications_sealed_.store(true, std::memory_order_seq_cst);
    }
    NotifyWaiters();
    return true;
}

bool RetirementControl::Resume(const Ticket& ticket) {
    std::lock_guard guard(state_mutex_);
    if (terminal_ || !publications_sealed_ || CheckDrain(ticket) != DrainStatus::Ready) return false;
    for (auto* entry = exit_registrations_; entry; entry = entry->next_) {
        std::atomic_ref<u64>(*entry->request_)
                .fetch_and(~kRetirementExitRequest, std::memory_order_acq_rel);
        if (entry->poll_) entry->poll_->DisarmIfIdle(entry->request_);
    }
    publications_sealed_ = false;
    instruction_resume_from_ = ticket.generation - 1;
    generation_.store(ticket.generation + 1, std::memory_order_seq_cst);
    NotifyWaiters();
    return true;
}

bool RetirementControl::AuthorizeInstructionResume(const Lease& execution, u64 interrupted_generation) const {
    std::lock_guard guard(state_mutex_);
    bool current_thread = false;
    for (auto* entry = Lease::current_; entry; entry = entry->previous_)
        current_thread |= entry == &execution;
    const auto generation = generation_.load(std::memory_order_seq_cst);
    return current_thread && execution.admitted_ && execution.owner_.get() == this &&
           execution.kind_ == Kind::Execution && !(generation & 1) &&
           execution.generation_ == generation && instruction_resume_from_ == interrupted_generation &&
           interrupted_generation + 2 == generation;
}

} // namespace swift::runtime::backend
