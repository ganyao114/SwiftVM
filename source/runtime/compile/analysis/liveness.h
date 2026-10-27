#pragma once

#include <span>

#include "runtime/ir/args.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile {

class SemanticFacts;

// IR SetHost* positions, independent of a physical allocation attempt. Sorted
// by register and instruction so both large functions and tiny blocks use one
// compact representation without per-register trees or duplicated FPR maps.
class HostRegisterWrites final {
public:
    struct Write {
        u16 reg;
        u16 position;
    };
    [[nodiscard]] std::span<const Write> For(u16 reg) const;
    [[nodiscard]] bool Crosses(u16 reg, u32 after, u32 through) const;

private:
    friend class SemanticFacts;
    StackVector<Write, 8> writes;
};

// Base liveness on one numbered input. Block reads omit virtual BitCast uses;
// Function reads retain their existing operand/use-list semantics. This does
// not build reverse consumers, CFG fixed points or spill next-use positions.
class ValueLiveness final {
public:
    [[nodiscard]] u32 UseEnd(u32 id) const;
    [[nodiscard]] u32 FunctionEnd(u32 id) const;
    [[nodiscard]] u32 DefinitionBlockEnd(const ir::Inst* definition) const;
    [[nodiscard]] bool UsesStayInBlock(const ir::Block* block, ir::Value value) const;
    [[nodiscard]] const HostRegisterWrites& GPRWrites() const { return gpr_writes; }
    [[nodiscard]] const HostRegisterWrites& FPRWrites() const { return fpr_writes; }

private:
    friend class SemanticFacts;
    ValueLiveness() = default;
    const ir::HIRFunction* function{};
    StackVector<u16, 64> use_ends;
    // Only use-list ends exceeding current operands need additional storage.
    // Aliases and manually constructed HIR can still have such entries.
    Vector<std::pair<u32, u32>> recorded_ends;
    Vector<u8> block_local;
    HostRegisterWrites gpr_writes, fpr_writes;
};

}  // namespace swift::runtime::compile
