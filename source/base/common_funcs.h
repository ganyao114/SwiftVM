#pragma once

#include "base/logging.h"
#include <string_view>
#include <type_traits>
#include <tuple>

namespace swift {

#define CONCAT2(x, y) DO_CONCAT2(x, y)
#define DO_CONCAT2(x, y) x##y

class DeleteCopyAndMove {
public:
    DeleteCopyAndMove(const DeleteCopyAndMove&) = delete;
    DeleteCopyAndMove(DeleteCopyAndMove&&) = delete;
    virtual ~DeleteCopyAndMove() = default;
    DeleteCopyAndMove& operator=(const DeleteCopyAndMove&) = delete;
    DeleteCopyAndMove& operator=(DeleteCopyAndMove&&) = delete;

protected:
    DeleteCopyAndMove() = default;
};

#define DECLARE_ENUM_FLAG_OPERATORS(type)                                                          \
    [[nodiscard]] constexpr type operator|(type a, type b) noexcept {                              \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<type>(static_cast<T>(a) | static_cast<T>(b));                           \
    }                                                                                              \
    [[nodiscard]] constexpr type operator&(type a, type b) noexcept {                              \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<type>(static_cast<T>(a) & static_cast<T>(b));                           \
    }                                                                                              \
    [[nodiscard]] constexpr type operator^(type a, type b) noexcept {                              \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<type>(static_cast<T>(a) ^ static_cast<T>(b));                           \
    }                                                                                              \
    [[nodiscard]] constexpr type operator<<(type a, type b) noexcept {                             \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<type>(static_cast<T>(a) << static_cast<T>(b));                          \
    }                                                                                              \
    [[nodiscard]] constexpr type operator>>(type a, type b) noexcept {                             \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<type>(static_cast<T>(a) >> static_cast<T>(b));                          \
    }                                                                                              \
    constexpr type& operator|=(type& a, type b) noexcept {                                         \
        a = a | b;                                                                                 \
        return a;                                                                                  \
    }                                                                                              \
    constexpr type& operator&=(type& a, type b) noexcept {                                         \
        a = a & b;                                                                                 \
        return a;                                                                                  \
    }                                                                                              \
    constexpr type& operator^=(type& a, type b) noexcept {                                         \
        a = a ^ b;                                                                                 \
        return a;                                                                                  \
    }                                                                                              \
    constexpr type& operator<<=(type& a, type b) noexcept {                                        \
        a = a << b;                                                                                \
        return a;                                                                                  \
    }                                                                                              \
    constexpr type& operator>>=(type& a, type b) noexcept {                                        \
        a = a >> b;                                                                                \
        return a;                                                                                  \
    }                                                                                              \
    [[nodiscard]] constexpr type operator~(type key) noexcept {                                    \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<type>(~static_cast<T>(key));                                            \
    }                                                                                              \
    [[nodiscard]] constexpr bool True(type key) noexcept {                                         \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<T>(key) != 0;                                                           \
    }                                                                                              \
    [[nodiscard]] constexpr bool False(type key) noexcept {                                        \
        using T = std::underlying_type_t<type>;                                                    \
        return static_cast<T>(key) == 0;                                                           \
    }

constexpr bool EndWith(std::string_view str, std::string_view suffix) {
    return str.size() >= suffix.size() &&
           0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix);
}

template <typename Container, typename T>
bool ContainsElement(const Container& container, const T& value, size_t start_pos = 0u) {
    ASSERT(start_pos < container.size());
    auto start = container.begin();
    std::advance(start, start_pos);
    auto it = std::find(start, container.end(), value);
    return it != container.end();
}

#define ENUM_TO_STRING_CASE(r) case ENUM_CLASS::r: return #r;
#define ENUM_DEFINE(r) r,

template <typename T> struct Identity {
    using type = T;
};

template<typename T>
constexpr T RoundDown(T x, typename Identity<T>::type n) {
    return (x & -n);
}

template<typename T>
constexpr T RoundUp(T x, std::remove_reference_t<T> n) {
    return RoundDown(x + n - 1, n);
}


template<class... E>
struct list {};

template<class F>
struct function_info : function_info<decltype(&F::operator())> {};

template<class R, class... As>
struct function_info<R(As...)> {
    using return_type = R;
    using parameter_list = list<As...>;
    static constexpr std::size_t parameter_count = sizeof...(As);

    using equivalent_function_type = R(As...);

    template<std::size_t I>
    struct parameter {
        static_assert(I < parameter_count, "Non-existent parameter");
        using type = std::tuple_element_t<I, std::tuple<As...>>;
    };
};

template<class F>
using equivalent_function_type = typename function_info<F>::equivalent_function_type;

template<class Function>
inline auto FuncPtrCast(Function f) noexcept {
    return static_cast<equivalent_function_type<Function>*>(f);
}

}