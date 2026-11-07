#pragma once

#include <fmt/format.h>
#include <utility>

namespace swift {
template <class... Args> std::string Format(fmt::format_string<Args...> pattern, Args&&... args) {
    return fmt::format(pattern, std::forward<Args>(args)...);
}

template <class Output, class... Args>
auto FormatTo(Output output, fmt::format_string<Args...> pattern, Args&&... args) {
    return fmt::format_to(output, pattern, std::forward<Args>(args)...);
}
}  // namespace swift
