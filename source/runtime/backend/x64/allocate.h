#pragma once
#include "runtime/backend/code.h"
#include "runtime/ir/program.h"

namespace swift::runtime::backend::x64 {
struct CompileOptions;

struct Home {
    std::uint32_t definition_position{}, last_use{}, stack_offset{}, auxiliary_offset{UINT32_MAX};
    std::uint8_t reg{}, flag_reads{}, flag_writes{}, flags_need{};
    std::uint32_t uniform_offset{UINT32_MAX};
    std::uint16_t use_count{};
    bool spilled{}, terminal_only{};
    // A stored value can use its adjacent architectural write as backing.
    std::uint8_t uniform_store_bytes{};
    bool reuse_rhs{}, rematerialize{};
};

struct Allocation {
    backend::CompileError error{};
    ir::NodeId node{};
    ir::BlockId block{};
    std::vector<Home> homes;
    std::vector<std::uint32_t> block_order;
    std::vector<std::uint8_t> flags_input;
    std::vector<bool> native_entry;
    std::vector<std::uint8_t> terminal_conditions;
    std::uint32_t spill_bytes{}, call_offset{};
    std::uint32_t flags_slot{UINT32_MAX};
    std::uint32_t condition_slot{UINT32_MAX};
    std::uint32_t cold_fp_count{}, cold_event_count{};
    bool has_calls{}, fpcr_may_change{}, uses_fpcr{};

    explicit operator bool() const noexcept { return error == backend::CompileError::None; }
};

Allocation AllocateBlock(const ir::Program&, unsigned pinned_slots = 0);
Allocation AllocateFunction(const ir::Program&, unsigned pinned_slots = 0);
void AllocateBlock(const ir::Program&,
                   Allocation&,
                   unsigned pinned_slots = 0,
                   const CompileOptions* = nullptr);
void AllocateFunction(const ir::Program&,
                      Allocation&,
                      unsigned pinned_slots = 0,
                      const CompileOptions* = nullptr);
}  // namespace swift::runtime::backend::x64
