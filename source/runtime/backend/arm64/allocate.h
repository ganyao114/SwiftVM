#pragma once

#include <cstdint>
#include <vector>

#include "runtime/ir/program.h"
#include "runtime/backend/code.h"

namespace swift::runtime::backend::arm64 {

struct CompileOptions;

using backend::CompileError;

// The one actual lifetime/location decision for each source node. A register
// home stays fixed throughout the unit; spills are stored at the definition.
// Scalar and V128 banks are disjoint; vector spills occupy aligned 16B.
// Linked scalar banks append x28 to the requested baseline register set.
struct Home {
    std::uint32_t definition_position{}, last_use{}, stack_offset{};
    std::uint8_t reg{};
    bool spilled : 1 {};
    bool terminal_only : 1 {};       // One final predicate/PC-literal consumer; no scalar is
                                     // materialized.
    std::uint8_t narrow_pin : 2 {};  // 0: canonical; 1/2/3: borrowed low8/16/32 pin.
    std::uint8_t nzcv_clobber
            : 4 {};                    // Unobserved NZCV fields the actual local writer may clobber.
    std::uint16_t extra_slot : 15 {};  // Div128 remainder: 8B slot index + 1; zero otherwise.
    bool flags_dead_after : 1 {};      // All guest flags are overwritten before any later observer.

    unsigned CarrierBits() const noexcept { return narrow_pin ? 4u << narrow_pin : 64u; }
};

static_assert(sizeof(Home) == 16);

struct Allocation {
    CompileError error{CompileError::None};
    ir::NodeId node{};
    ir::BlockId block{};
    std::vector<Home> homes;
    std::vector<std::uint32_t> block_order;  // One actual CFG reverse-postorder schedule.
    std::uint32_t spill_bytes{};
    std::uint32_t used_vector_mask{};
    bool publish_entry_pc{};  // This unit needs canonical incoming PC at entry.
    bool has_save_flags{};

    explicit operator bool() const noexcept { return error == CompileError::None; }
};

// Input producers must supply defined control-flow values: a definition must
// dominate each executed use. Earlier birth alone does not prove dominance;
// these entry points do not implement an SSA/dominance validator. Phi is not
// supported by the code writer.
Allocation AllocateBlock(const ir::Program&, const CompileOptions* options = nullptr);
Allocation AllocateFunction(const ir::Program&, const CompileOptions* options = nullptr);
// Reuse only this actual allocation owner's capacity. Every request resets
// homes/order and scalar decisions; no Program or Writer borrow is retained.
void AllocateBlock(const ir::Program&, Allocation&, const CompileOptions* options = nullptr);
void AllocateFunction(const ir::Program&, Allocation&, const CompileOptions* options = nullptr);

}  // namespace swift::runtime::backend::arm64
