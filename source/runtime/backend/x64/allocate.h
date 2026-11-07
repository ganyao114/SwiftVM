#pragma once
#include "runtime/backend/code.h"
#include "runtime/ir/program.h"

namespace swift::runtime::backend::x64 {
struct Home {
    std::uint32_t definition_position{}, last_use{}, stack_offset{}, auxiliary_offset{UINT32_MAX};
    std::uint8_t reg{};
    bool spilled{};
};

struct Allocation {
    backend::CompileError error{};
    ir::NodeId node{};
    ir::BlockId block{};
    std::vector<Home> homes;
    std::vector<std::uint32_t> block_order;
    std::uint32_t spill_bytes{}, call_offset{};
    bool has_calls{};

    explicit operator bool() const noexcept { return error == backend::CompileError::None; }
};

Allocation AllocateBlock(const ir::Program&);
Allocation AllocateFunction(const ir::Program&);
void AllocateBlock(const ir::Program&, Allocation&);
void AllocateFunction(const ir::Program&, Allocation&);
}  // namespace swift::runtime::backend::x64
