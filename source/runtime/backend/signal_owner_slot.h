#pragma once

#include <atomic>
#include <thread>
#include "runtime/common/types.h"

namespace swift::runtime::backend {

// The TLS owner keeps the slot alive. Readers pin an already loaded owner
// until the detaching thread has observed their completion.
class SignalOwnerSlot {
public:
    static_assert(std::atomic<void*>::is_always_lock_free);
    static_assert(std::atomic<u64>::is_always_lock_free);

    class Reader {
    public:
        explicit Reader(SignalOwnerSlot* slot) noexcept : slot_(slot) {
            if (!slot_) return;
            slot_->readers_.fetch_add(1, std::memory_order_seq_cst);
            owner_ = slot_->owner_.load(std::memory_order_seq_cst);
        }
        ~Reader() {
            if (slot_) slot_->readers_.fetch_sub(1, std::memory_order_seq_cst);
        }
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;
        [[nodiscard]] void* Get() const { return owner_; }

    private:
        SignalOwnerSlot* slot_{};
        void* owner_{};
    };

    [[nodiscard]] bool Publish(void* owner) noexcept {
        if (!owner) return false;
        void* expected{};
        return owner_.compare_exchange_strong(expected, owner, std::memory_order_seq_cst);
    }

    // Not signal-safe: call only from the owner destructor, outside any reader.
    void DetachAndWait(void* owner) noexcept {
        owner_.compare_exchange_strong(owner, nullptr, std::memory_order_seq_cst);
        while (readers_.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
    }

    [[nodiscard]] bool IsPublished() const noexcept {
        return owner_.load(std::memory_order_seq_cst) != nullptr;
    }

private:
    std::atomic<void*> owner_{};
    std::atomic<u64> readers_{};
};

} // namespace swift::runtime::backend
