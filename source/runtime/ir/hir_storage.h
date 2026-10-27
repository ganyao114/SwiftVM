#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::ir {
struct HIRPools;
class HIRFunction;
class HIRBlock;

// Shares one pool session. Idle storage returns to its originating thread's
// cache; live sessions remain valid after the builder or that thread ends.
class HIRPoolLease final {
public:
    HIRPoolLease() noexcept = default;
    explicit HIRPoolLease(u32 function_capacity);
    ~HIRPoolLease();
    HIRPoolLease(const HIRPoolLease& other);
    HIRPoolLease& operator=(const HIRPoolLease& other);
    HIRPoolLease(HIRPoolLease&& other) noexcept;
    HIRPoolLease& operator=(HIRPoolLease&& other) noexcept;

    void Reset() noexcept;
    [[nodiscard]] HIRPools& Get() const;

private:
    friend class HIRFunction;
    friend class HIRBlock;
    static HIRPoolLease Retain(HIRPools& pools);
    explicit HIRPoolLease(HIRPools* pools) : pools(pools) {}
    HIRPools* pools{};
};
}  // namespace swift::runtime::ir
