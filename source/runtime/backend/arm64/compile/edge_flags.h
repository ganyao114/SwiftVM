#pragma once

#include <functional>
#include <span>
#include <vector>
#include "runtime/backend/edge_flags_state.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {
EdgeFlagsTargetContract AnalyzeBlockEdgeFlags(const ir::Block& block,
                                              const std::function<bool(const ir::Inst&)>& observes);
bool AnalyzeBlockFlagsTransparent(const ir::Block& block, bool* writes_flags = nullptr);
bool AnalyzeBlockTokenOverwrite(const ir::Block& block,
                                const std::function<bool(const ir::Inst&)>& barrier);

// Bit m records acceptance of incoming NZCV nibble m. A transparent direct
// successor can accept the masks left unresolved by this block.
u16 AnalyzeBlockEdgeAcceptance(const ir::Block& block);
bool MayQueryEdgeAcceptance(const ir::Block& block);
bool HasConditionalEdgeTerminal(const ir::Terminal& terminal);
struct EdgeAcceptanceNode {
    u16 accepted{};
    u32 successor{UINT32_MAX};
};
std::vector<u16> ResolveEdgeAcceptance(std::span<const EdgeAcceptanceNode> nodes);
}  // namespace swift::runtime::backend::arm64
