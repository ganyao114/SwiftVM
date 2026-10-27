#pragma once

#include "runtime/ir/args.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
Value ResolveBitCastSource(Value value);
}

namespace swift::runtime::compile {

class BlockAnalysisIndex;

struct FlagsDemand {
    ir::Flags requested{};
    u16 pseudo_count{};
    bool branch_only{};
    bool has_save{};
};

// Dense over local definitions, sparse for cross-block sources. Caching B
// blocks must not allocate B * unit-size entries.
class BlockUseEnds final {
public:
    BlockUseEnds(const ir::Block* block,
                 u32 instruction_count,
                 const BlockAnalysisIndex* consumers = nullptr);
    [[nodiscard]] u32 size() const { return instruction_count; }
    [[nodiscard]] u32 operator[](u32 value_id) const;
    [[nodiscard]] size_t StoredValueCount() const { return local.size() + external.size(); }

private:
    void Record(ir::Value value, u32 position);
    u32 instruction_count{};
    u32 first{};
    Vector<u32> local;
    Vector<std::pair<u32, u32>> external;
};

}  // namespace swift::runtime::compile
