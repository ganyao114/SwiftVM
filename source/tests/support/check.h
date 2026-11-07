#pragma once

#include <source_location>
#include <string_view>
#include <catch2/catch_test_macros.hpp>

namespace swift::tests {
// Migration adapter for the existing numerical assertions. Catch owns case
// registration, failure handling and reporting; this preserves call-site
// messages without rewriting the tested expressions or their read order.
inline void Check(bool condition,
                  std::string_view message,
                  std::source_location location = std::source_location::current()) {
    if (condition) return;
    INFO(location.file_name() << ':' << location.line() << ": " << message);
    REQUIRE(condition);
}

inline void Check(bool condition,
                  const char* expression,
                  int line,
                  std::source_location location = std::source_location::current()) {
    if (condition) return;
    INFO(location.file_name() << ':' << line << ": " << expression);
    REQUIRE(condition);
}

inline void Check(bool condition,
                  int line,
                  std::source_location location = std::source_location::current()) {
    Check(condition, "contract expression", line, location);
}
}  // namespace swift::tests

// Existing expressions include comma-containing template arguments and
// boolean chains. Evaluate them once before Catch's failure adapter, retaining
// the successful assertion's original zero-allocation behavior.
#undef CHECK
#define CHECK(...) ::swift::tests::Check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)
