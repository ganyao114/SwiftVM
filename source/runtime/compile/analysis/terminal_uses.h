#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
class Inst;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile {

// Read-only membership in the terminal values of one Block or the executable
// RPO members of a Function. Hoist anchors are separate implicit roots. The
// caller chooses raw identities or resolved BitCast roots; physical mappings
// and register widths are never inferred here.
class TerminalValueUses final {
public:
    enum class Aliases : u8 { Raw, Resolved };
    TerminalValueUses(const ir::Block*, Aliases);
    TerminalValueUses(const ir::HIRFunction*, Aliases);
    TerminalValueUses(const TerminalValueUses&) = delete;
    TerminalValueUses& operator=(const TerminalValueUses&) = delete;
    [[nodiscard]] bool Contains(const ir::Inst*) const;

private:
    void Append(const ir::Block*, Aliases);
    StackVector<const ir::Inst*, 4> values;
};

}  // namespace swift::runtime::compile
