#include "call_preparation.h"

#include <map>
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/frontend/x86/sse42str_helper.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {

std::vector<u64> PrepareSharedHostCalls(const ir::HIRFunction& function,
                                        compile::SemanticFacts& facts) {
    ASSERT(facts.MatchesInput(nullptr, &function));
    // Only the second occurrence matters. Count all raw occurrences, as the
    // old Function emitter did, including calls later covered by an action.
    std::map<u64, bool> repeated;
    for (const auto& member : function.GetHIRBlocksRPO()) {
        const auto* block = member.GetBlock();
        if (!facts.PrepareSummary(block).HasHostCallCandidates()) continue;
        for (const auto& inst : block->GetInstList()) {
            u64 target{};
            if (inst.GetOp() == ir::OpCode::CallLambda) {
                const auto lambda = inst.GetArg<ir::Lambda>(0);
                if (!lambda.IsValue()) target = lambda.GetImm().Get();
            } else if (inst.GetOp() == ir::OpCode::Sse42Str) {
                target = swift::x86::Sse42StrVectorHelperAddress(u8(inst.GetArg<ir::Imm>(2).Get()));
            }
            if (!target) continue;
            auto [entry, inserted] = repeated.try_emplace(target, false);
            if (!inserted) entry->second = true;
        }
    }
    std::vector<u64> targets;
    for (const auto& [target, shared] : repeated)
        if (shared) targets.push_back(target);
    return targets;
}

}  // namespace swift::runtime::backend::arm64
