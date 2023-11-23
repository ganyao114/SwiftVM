//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::ir {

class Location {
public:
    constexpr Location() = default;

    constexpr Location(size_t value) : value(value) {}

    bool operator==(const Location& o) const { return value == o.Value(); }

    bool operator!=(const Location& o) const { return !operator==(o); }

    [[nodiscard]] size_t Value() const { return value; }

private:
    size_t value{};
};

inline bool operator<(const Location& x, const Location& y) noexcept {
    return x.Value() < y.Value();
}

inline bool operator>(const Location& x, const Location& y) noexcept {
    return x.Value() > y.Value();
}

std::string ToString(const Location& descriptor);

}  // namespace swift::runtime::ir

namespace std {
template<>
struct less<swift::runtime::ir::Location> {
    bool operator()(const swift::runtime::ir::Location& x, const swift::runtime::ir::Location& y) const noexcept {
        return x < y;
    }
};
template<>
struct hash<swift::runtime::ir::Location> {
    size_t operator()(const swift::runtime::ir::Location& x) const noexcept {
        return std::hash<size_t>()(x.Value());
    }
};
}  // namespace std