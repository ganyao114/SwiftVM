#pragma once

#include "runtime/ir/direct/services.h"
#include <cstddef>
#include <cstdint>

namespace swift::runtime::frontend {

struct CodeReadResult {
    ir::direct::AccessResult access;
    std::size_t bytes{};
};

enum class StepKind : std::uint8_t {
    Completed,
    Service,
    Fault,
    Unsupported,
    Incomplete,
    InvalidEncoding,
    Truncated,
};

struct StepResult {
    StepKind kind{StepKind::Completed};
    std::uint64_t pc{}, next_pc{}, fault_address{}, service{};
    std::size_t instructions{}, decoded_bytes{};
    bool code_changed{};
};

}  // namespace swift::runtime::frontend
