#pragma once

#include <atomic>
#include <utility>
#include <vector>
#include "runtime/common/types.h"

namespace swift::runtime::backend {

// Each installed unit owns a distinct token even when its address is reused.
// Retained aliases share this token, never the allocation's later occupant.
class CodeLifetime final {
public:
    CodeLifetime(u8* base, size_t size, std::vector<VAddr> entries)
            : base(base), size(size), entries(std::move(entries)) {}

    [[nodiscard]] bool Commit() noexcept {
        auto expected = State::Pending;
        return state.compare_exchange_strong(expected, State::Live, std::memory_order_seq_cst);
    }
    void Revoke() noexcept { state.store(State::Revoked, std::memory_order_seq_cst); }
    [[nodiscard]] bool Live() const noexcept {
        return state.load(std::memory_order_seq_cst) == State::Live;
    }
    [[nodiscard]] bool Revoked() const noexcept {
        return state.load(std::memory_order_seq_cst) == State::Revoked;
    }

    u8* const base;
    const size_t size;
    const std::vector<VAddr> entries;

private:
    enum class State { Pending, Live, Revoked };
    static_assert(std::atomic<State>::is_always_lock_free);
    std::atomic<State> state{State::Pending};
};

}  // namespace swift::runtime::backend
