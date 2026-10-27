#pragma once

#include <functional>
#include "lower_flags.h"
#include "runtime/ir/terminal.h"

namespace swift::runtime::compile {
class CompileWorkspace;
}
namespace swift::runtime::backend::arm64 {

// Temporary preparation output. Frozen units keep only the compact rows and
// optional audit payload, never pointer maps, use graphs or PSTATE prefixes.
struct ConsumerActions final {
    bool valid{true};
    std::vector<ConsumerAction> actions;
    std::vector<AluSplitAction> audit;
    std::optional<PstateEligibility> pstate;
    [[nodiscard]] const ConsumerAction* Find(u32 id) const;
    [[nodiscard]] const AluSplitFacts* Audit(u32 id) const;
    [[nodiscard]] bool Has(u32 id, ConsumerFlag bit) const {
        const auto* row = Find(id);
        return row && row->Has(bit);
    }
    void AddIncomingZeros(compile::BlockAnalysisIndex&, std::span<const ir::Inst* const>);
};
// Explicitly mutable preparation: only an observed edge query requests the
// shared direct-use graph. The numbered IR and const edge queries stay read-only.
ConsumerActions LowerConsumers(compile::BlockAnalysisIndex&,
                               compile::CompileWorkspace&,
                               const std::function<bool(const ir::Inst&)>& retains_nzcv,
                               const std::function<bool(const ir::Inst&)>& observes,
                               const ir::Terminal* terminal = nullptr,
                               bool alu_split_audit = false);
}  // namespace swift::runtime::backend::arm64
