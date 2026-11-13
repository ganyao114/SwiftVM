#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include "runtime/ir/program.h"

namespace swift::runtime::ir {

// The caller's actual flags storage in the Uniform buffer. No backend flag
// bit layout is exposed here. Unknown placement keeps the broad alias barrier.
struct UniformRange {
    std::uint32_t offset{}, bytes{};
};

struct UniformPolicy {
    std::optional<UniformRange> flags;
    // Actual execution ownership: valid guest-memory addresses cannot name
    // this private state buffer. Generic clients keep the aliasing default.
    bool guest_memory_disjoint{};
    // Implicit numerical status writes affect this control/status field only.
    std::optional<UniformRange> numeric_control;
    std::optional<UniformRange> fp_status;
    std::optional<UniformRange> pc;
};

struct OptimizeStats {
    std::size_t nodes{}, value_uses{}, folded{}, copy_forwarded{}, uniform_forwarded{}, erased{};
    std::size_t uniform_stores_erased{};
    std::size_t dense_bytes{}, words_before{}, words_after{};
    // Selected Function CFG buffer capacities, including full local arrays
    // for small graphs; zero when no flag graph is needed. Not heap bytes.
    std::size_t block_flags_bytes{};
    std::size_t final_walk_skipped{};  // 0/1: the earlier walks left no final cleanup to do.
};

struct OptimizeResult {
    Error error{Error::None};
    Program program;
    OptimizeStats stats;
    // After taking ownership, allocation failure returns this same, possibly
    // partly optimized owner with CapacityOverflow. Every completed edit is
    // equivalent; no clone or all-program rollback is performed.
    bool allocation_failed{};

    explicit operator bool() const noexcept { return error == Error::None; }
};

// At most three bounded walks over the original storage, with one u64 row per node.
// IDs, blocks, terminals, target metadata and natural result types stay fixed.
// Both reverse walks couple data uses to flags demand. Block exits, external
// Function exits, faults and opaque observations retain all flags. Function
// summaries and terminal edges conservatively refine internal exits between
// walks, without another node scan. The last walk uses those frozen exits;
// it is omitted only when the earlier walks prove it would leave the graph
// and flag metadata unchanged, not from previously reported optimization counts.
// This does not claim a joint global flags/DCE fixed point.
// The editor's absolute arena limit is min(old words + 2*nodes, caller limit).
// Each rejected edit is atomic. An error still returns the one valid owner.
// Allocation of the dense table occurs before ownership transfer and can
// throw bad_alloc, leaving the caller's input unchanged.
OptimizeResult Optimize(Program&&,
                        std::size_t max_operand_words = std::numeric_limits<std::size_t>::max(),
                        UniformPolicy uniforms = {});

// Borrow the same use/flag rows across requests. Their contents are reset
// before taking Program ownership; no Program reference survives the call.
OptimizeResult Optimize(Program&&,
                        std::vector<std::uint64_t>& rows,
                        std::size_t max_operand_words = std::numeric_limits<std::size_t>::max(),
                        UniformPolicy uniforms = {});

}  // namespace swift::runtime::ir
