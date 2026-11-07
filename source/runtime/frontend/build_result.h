#pragma once

#include "runtime/ir/program.h"

namespace swift::runtime::frontend {

enum class BuildError : std::uint8_t {
    None,
    InvalidInput,
    InvalidEncoding,
    Truncated,
    Unsupported,
    IRRejected,
    FormationRejected,
};

// The real submitted Program, not another operation representation.
struct BuildResult {
    BuildError error{BuildError::None};
    std::uint64_t error_pc{};
    std::size_t decoded_bytes{}, instruction_count{};
    ir::Error ir_error{ir::Error::None};
    ir::Program program;

    explicit operator bool() const noexcept { return error == BuildError::None && bool(program); }
};

}  // namespace swift::runtime::frontend
