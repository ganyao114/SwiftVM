#include "terminal_uses.h"

#include <algorithm>
#include <functional>

#include "facts.h"
#include "runtime/ir/hir_builder.h"
#include "uses.h"

namespace swift::runtime::compile {

TerminalValueUses::TerminalValueUses(const ir::Block* block, Aliases aliases) {
    ASSERT(block);
    Append(block, aliases);
}

TerminalValueUses::TerminalValueUses(const ir::HIRFunction* function, Aliases aliases) {
    ASSERT(function);
    for (const auto& member : function->GetHIRBlocksRPO()) Append(member.GetBlock(), aliases);
}

void TerminalValueUses::Append(const ir::Block* block, Aliases aliases) {
    VisitTerminalValues(block->GetTerminal(), [&](ir::Value value) {
        if (aliases == Aliases::Resolved) value = ir::ResolveBitCastSource(value);
        if (!value.Defined()) return;
        const auto at = std::lower_bound(
                values.begin(), values.end(), value.Def(), std::less<const ir::Inst*>{});
        // Repeated occurrences never grow storage. Small terminal sets stay
        // inline; larger inputs keep one pointer per distinct definition.
        if (at == values.end() || *at != value.Def()) values.insert(at, value.Def());
    });
}

bool TerminalValueUses::Contains(const ir::Inst* definition) const {
    return std::binary_search(
            values.begin(), values.end(), definition, std::less<const ir::Inst*>{});
}

}  // namespace swift::runtime::compile
