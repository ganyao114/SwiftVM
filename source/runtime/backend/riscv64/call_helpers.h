#pragma once

#include <cstdint>

namespace swift::runtime::backend::riscv64 {

struct CallPair {
    std::uint64_t first{}, second{};
};

// config carries the dividend-half width in bits0..7 and signedness in bit8.
// Failure is returned through valid; helpers never raise a host signal or
// unwind over generated code. Both successful results are zero-normalized.
CallPair WideDivide(std::uint64_t high,
                    std::uint64_t low,
                    std::uint64_t divisor,
                    unsigned config,
                    std::uint64_t* valid) noexcept;

// The metadata fields address the private architectural state around state.
// Pending exceptions retain their original IP/DP/opcode; a new exception
// commits the current instruction, as does a successful final helper.
std::uint64_t CheckedX87(void* state,
                         std::uint64_t command,
                         std::uint64_t raw,
                         std::uint64_t pc,
                         std::uint64_t dp,
                         std::uint64_t metadata) noexcept;

}  // namespace swift::runtime::backend::riscv64
