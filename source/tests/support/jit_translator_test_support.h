#pragma once
#include "runtime/backend/arm64/jit/translator.h"

namespace swift::runtime::backend::arm64 {
class JitTranslatorTestSupport {
public:
    static bool IsInstructionSuppressed(const JitTranslator& translator, const ir::Inst* inst) {
        return inst && inst->Id() < translator.disable_instructions.size() &&
               translator.disable_instructions.test(inst->Id());
    }
    static std::pair<size_t, size_t> ScalarPublicationCounts(const JitTranslator& translator) {
        const auto& plan = translator.ScalarPublicationPlan();
        return {plan.loads.size(), plan.values.size()};
    }
    // Isolated encoder tests supply frozen physical maps and bind semantic
    // metadata explicitly instead of running target allocation or block entry.
    static void BindPseudoFlags(JitTranslator& translator, ir::Block& block) {
        ASSERT(translator.context.GetAllocation().GPRLocationsFrozen());
        BlockAnalysisIndex index;
        index.Build(&block, nullptr, true);
        auto plan = std::make_shared<GPRBlockPlan>();
        plan->pseudo_flags = index.ReleasePseudoFlags();
        translator.gpr_plan_owner = std::move(plan);
        translator.gpr_plan = translator.gpr_plan_owner.get();
    }
};
}
