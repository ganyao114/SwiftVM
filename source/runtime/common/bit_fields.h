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

    template <class R> inline BitField& operator=(R val) {
        storage = (storage & ~mask) | (((T) val << position) & mask);
        return *this;
    }

    template <class R> inline bool operator==(R val) {
        return (R) ((storage & mask) >> position) == val;
    }

    inline T operator *() const { return (T)((storage & mask) >> position); }

    template <class R> inline R get() const { return (R)((storage & mask) >> position); }

private:
    BitField(T val) = delete;

    T storage;

    static constexpr T mask = ((~(T)0) >> (8 * sizeof(T) - bits)) << position;

    static_assert(!std::numeric_limits<T>::is_signed);
    static_assert(bits > 0);
};
}  // namespace swift::runtime
