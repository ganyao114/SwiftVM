#pragma once

#include <cstdint>

namespace swift::runtime {

// Original guest bytes consumed by decoding or semantic lookahead. This is
// source dependency metadata, independent of IR nodes and native entries.
struct SourceSpan {
    std::uint64_t pc{}, size{};
};

}  // namespace swift::runtime
