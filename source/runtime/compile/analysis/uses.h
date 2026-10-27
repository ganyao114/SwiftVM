#pragma once

#include <utility>

#include "runtime/ir/block.h"

namespace swift::runtime::compile {

// Raw terminal values only. Capture and width proofs choose their own alias
// policy; hoist anchors belong to the separate implicit-root wrapper below.
template <typename Visitor>
void VisitTerminalValues(const ir::Terminal& terminal, Visitor&& record) {
    ir::ForEachTerminalValue(terminal, std::forward<Visitor>(record));
}

// Terminals and hoist anchors are real uses without ordinary IR arguments.
// The caller supplies its position and decides which use representation to fill.
template <typename Visitor>
void VisitImplicitUses(const ir::Block& block, u32 position, Visitor&& record) {
    VisitTerminalValues(block.GetTerminal(), [&](ir::Value value) { record(value, position); });
    for (auto* anchor : block.GetLoopHoistMetadata().anchors) record(ir::Value{anchor}, position);
}

// Admission and final SSA/use-end collection share the same terminal walk.
// Hoist anchors have no terminal-specific integer-domain restriction.
template <typename Visitor>
bool CheckImplicitUses(const ir::Block& block, u32 position, Visitor&& record) {
    bool valid = ir::CheckTerminal(block.GetTerminal(),
            [&](ir::Value value) { record(value, position); });
    for (auto* anchor : block.GetLoopHoistMetadata().anchors) {
        const ir::Value value{anchor};
        if (value.HasValidDataView()) record(value, position);
        else valid = false;
    }
    return valid;
}

}  // namespace swift::runtime::compile
