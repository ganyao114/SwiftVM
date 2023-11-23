#pragma once

#include <cstddef>
#include "types.h"

namespace swift::runtime {

template <typename T> constexpr T AlignUp(T value, size_t size) {
    auto mod{static_cast<T>(value % size)};
    value -= mod;
    return static_cast<T>(mod == T{0} ? value : value + size);
}

template <typename T> constexpr T AlignDown(T value, size_t size) {
    return static_cast<T>(value - value % size);
}

}  // namespace swift::runtime
