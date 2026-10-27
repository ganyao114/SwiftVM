#pragma once

#include <span>
#include <utility>
#include <vector>

#include "runtime/common/types.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
class Inst;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile::audit {

enum class FailureKind {
    None,
    Blocks,
    CFG,
    Numbering,
    Owner,
    Arguments,
    Terminal,
    LocalControl,
    Dominance,
};

// An independent, optional reference for the actual IR. It borrows the IR for
// this audit invocation only, and never reads or prepares production facts.
// Function inputs name exactly its executable RPO blocks. Other members still
// receive shape/ownership checks, without reachable-path SSA requirements.
class IRReference final {
public:
    IRReference(std::span<const ir::Block* const> emitted_blocks,
                const ir::HIRFunction* function,
                u32 instruction_capacity);

    [[nodiscard]] bool Valid() const { return failure == FailureKind::None; }
    [[nodiscard]] FailureKind Failure() const { return failure; }
    [[nodiscard]] const ir::Block* Owner(const ir::Inst* instruction) const;
    [[nodiscard]] bool Dominates(const ir::Block* definition, const ir::Block* use) const;

private:
    struct Definition {
        const ir::Inst* instruction;
        const ir::Block* block;
        u32 position;
    };

    [[nodiscard]] bool Reject(FailureKind reason);
    [[nodiscard]] bool BuildGraph(std::span<const ir::Block* const> emitted_blocks,
                                  const ir::HIRFunction* function,
                                  std::vector<const ir::Block*>& members);
    [[nodiscard]] bool Verify(std::span<const ir::Block* const> members,
                              u32 instruction_capacity);
    [[nodiscard]] const Definition* Find(const ir::Inst* instruction) const;
    [[nodiscard]] u32 BlockIndex(const ir::Block* block) const;

    static constexpr u32 missing = UINT32_MAX;
    FailureKind failure{FailureKind::None};
    std::vector<Definition> definitions;
    std::vector<std::pair<const ir::Block*, u32>> block_ids;
    std::vector<std::vector<u64>> dominators;
};

}  // namespace swift::runtime::compile::audit
