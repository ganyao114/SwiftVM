//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/common/types.h"
#include "runtime/include/config.h"

namespace swift::runtime::ir {

class Location {
public:
    constexpr static LocationDescriptor INVALID{LocationDescriptor(-1)};

    constexpr Location() = default;

    constexpr Location(LocationDescriptor value) : value(value) {}

    bool operator==(const Location& o) const { return value == o.Value(); }

    bool operator!=(const Location& o) const { return !operator==(o); }

    bool operator>=(const LocationDescriptor& loc) const { return loc >= value; }

    bool operator<=(const LocationDescriptor& loc) const { return loc <= value; }

    void operator=(LocationDescriptor loc) { value = loc; }

    LocationDescriptor operator*() const { return value; }

    Location operator+(LocationDescriptor size) const {
        auto result{*this};
        result.value += size;
        return result;
    }

    [[nodiscard]] LocationDescriptor Value() const { return value; }

    [[nodiscard]] bool Valid() const { return value != INVALID; }

private:
    LocationDescriptor value{};
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