#pragma once

#include <cstdint>

namespace swift::runtime {
template <std::size_t position, std::size_t bits, typename T> struct BitField {
public:
    static constexpr uint32_t pos = position;
    static constexpr uint32_t len = bits;

    BitField() = default;

    inline BitField& operator=(T val) {
        storage = (storage & ~mask) | ((val << position) & mask);
        return *this;
    }

    inline operator T() const { return (T)((storage & mask) >> position); }

private:
    BitField(T val) = delete;

    T storage;

    static constexpr T mask = ((~(T)0) >> (8 * sizeof(T) - bits)) << position;

    static_assert(!std::numeric_limits<T>::is_signed);
    static_assert(bits > 0);
};
}  // namespace swift::runtime
