#pragma once

#include <optional>
#include <span>
#include <utility>

#include "runtime/common/types.h"

namespace swift::runtime::ir {
class Block;
class HIRBlock;
class HIRFunction;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile {

class SemanticFacts;

// Check reference identity before reading a referenced HIR block. The owned
// roster is authoritative; cached predecessor/successor rows must agree with
// their original edge lists, including order and multiplicity.
[[nodiscard]] bool FunctionCFGClosed(const ir::HIRFunction& function);

// The executable RPO and its original HIR edges. Missing members remain
// explicit; each consumer decides how an external predecessor or edge affects
// its own dataflow. This does not replace terminal-derived control flow.
class FunctionCFG final {
public:
    static constexpr u32 MissingBlock = UINT32_MAX;

    FunctionCFG(const FunctionCFG&) = delete;
    FunctionCFG& operator=(const FunctionCFG&) = delete;

    struct BlockInfo {
        const ir::HIRBlock* hir{};
        u32 predecessor_begin{}, predecessor_count{};
        u32 successor_begin{}, successor_count{};
        bool function_entry{}, external_entry{}, has_incoming_edges{}, call_return{};
        u32 return_block{MissingBlock};
    };

    [[nodiscard]] std::span<const BlockInfo> Blocks() const { return blocks; }
    [[nodiscard]] std::span<const u32> Predecessors(const BlockInfo& block) const {
        return std::span<const u32>{edges}.subspan(block.predecessor_begin,
                                                   block.predecessor_count);
    }
    [[nodiscard]] std::span<const u32> Successors(const BlockInfo& block) const {
        return std::span<const u32>{edges}.subspan(block.successor_begin, block.successor_count);
    }
    [[nodiscard]] std::optional<u32> FindBlock(const ir::Block* block) const;
    [[nodiscard]] bool Matches(const ir::HIRFunction& function) const;

private:
    friend class SemanticFacts;
    explicit FunctionCFG(const ir::HIRFunction& function);

    Vector<BlockInfo> blocks;
    Vector<u32> edges;
    Vector<std::pair<const ir::Block*, u32>> block_ids;
};

}  // namespace swift::runtime::compile
