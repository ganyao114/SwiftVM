#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include "runtime/ir/program.h"

namespace swift::runtime::compile {

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
};

struct OptimizeStats {
    std::size_t nodes{}, value_uses{}, folded{}, copy_forwarded{}, uniform_forwarded{}, erased{};
    std::size_t dense_bytes{}, words_before{}, words_after{};
    std::size_t block_flags_bytes{};  // Function-only summaries, actual predecessor edges and
                                      // bounded queue.
};

struct OptimizeResult {
    ir::Error error{ir::Error::None};
    ir::Program program;
    OptimizeStats stats;
    // After taking ownership, allocation failure returns this same, possibly
    // partly optimized owner with CapacityOverflow. Every completed edit is
    // equivalent; no clone or all-program rollback is performed.
    bool allocation_failed{};
    explicit operator bool() const noexcept { return error == ir::Error::None; }
};

// Three bounded walks over the original storage, with one u64 row per node.
// IDs, blocks, terminals, target metadata and natural result types stay fixed.
// Both reverse walks couple data uses to flags demand. Block exits, external
// Function exits, faults and opaque observations retain all flags. Function
// summaries and terminal edges conservatively refine internal exits between
// walks, without another node scan. The last walk uses those frozen exits;
// it does not claim a joint global flags/DCE fixed point.
// The editor's absolute arena limit is min(old words + 2*nodes, caller limit).
// Each rejected edit is atomic. An error still returns the one valid owner.
// Allocation of the dense table occurs before ownership transfer and can
// throw bad_alloc, leaving the caller's input unchanged.
OptimizeResult Optimize(ir::Program&&,
                        std::size_t max_operand_words = std::numeric_limits<std::size_t>::max(),
                        UniformPolicy uniforms = {});

// Borrow the same use/flag rows across requests. Their contents are reset
// before taking Program ownership; no Program reference survives the call.
OptimizeResult Optimize(ir::Program&&,
                        std::vector<std::uint64_t>& rows,
                        std::size_t max_operand_words = std::numeric_limits<std::size_t>::max(),
                        UniformPolicy uniforms = {});

}  // namespace swift::runtime::compile
