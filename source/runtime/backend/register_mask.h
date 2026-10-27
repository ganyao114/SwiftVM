#pragma once

#include <bit>
#include "base/types.h"

namespace swift::runtime::backend {

template<typename T = u32>
class RegisterMask {
public:

    explicit RegisterMask() : mask() {}

    explicit RegisterMask(T mask) : mask(mask) {}

    auto GetFirstMarked() {
        return std::countr_zero(mask);
    }

    auto GetFirstClear() {
        constexpr auto bit_count = static_cast<int>(sizeof(T) * 8);
        auto idx = std::countr_one(mask);
        // All bits marked → no clear register; countr_one returns the
        // bit-width, which is not a valid register index.
        return idx < bit_count ? idx : -1;
    }

    auto GetMarkedCount() {
        return std::popcount(mask);
    }

    auto GetClearCount() {
        return GetAllCount() - std::popcount(mask);
    }

    auto GetAllCount() {
        return sizeof(T) * 8;
    }

    [[nodiscard]] bool Null() const {
        return mask == 0;
    }

    [[nodiscard]] bool Get(u32 bit) const {
        return mask & (T(1) << bit);
    }

    bool operator==(const RegisterMask&) const = default;

    void Mark(u32 bit) {
        mask |= (T(1) << bit);
    }

    void Clear(u32 bit) {
        mask &= ~(T(1) << bit);
    }

    void Reset(T value) {
        mask = value;
    }

private:
    T mask;
};

using GPRSMask = RegisterMask<u32>;
using FPRSMask = RegisterMask<u32>;

}  // namespace swift::runtime::backend
