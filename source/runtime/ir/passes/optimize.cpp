#include "runtime/ir/optimize.h"

#include <algorithm>
#include <new>
#include <utility>
#include "detail.h"

namespace swift::runtime::ir {
namespace {
OptimizeResult Finish(Builder& editor,
                      Error error,
                      OptimizeStats stats,
                      bool allocation_failed = false) {
    auto submitted = editor.Submit();
    if (submitted.error != Error::None) error = submitted.error;
    stats.words_after = submitted.program.OperandArena().size();
    return {error, std::move(submitted.program), stats, allocation_failed};
}
}  // namespace

OptimizeResult Optimize(Program&& input, std::size_t max_operand_words, UniformPolicy policy) {
    std::vector<std::uint64_t> rows;
    return Optimize(std::move(input), rows, max_operand_words, policy);
}

OptimizeResult Optimize(Program&& input,
                        std::vector<std::uint64_t>& rows,
                        std::size_t max_operand_words,
                        UniformPolicy policy) {
    if (!input) return {Error::Consumed, std::move(input), {}};
    OptimizeStats stats;
    stats.nodes = input.Operations().size();
    stats.words_before = input.OperandArena().size();
    rows.assign(stats.nodes, 0);  // Caller still owns input if this allocation fails.
    stats.dense_bytes = rows.size() * sizeof(std::uint64_t);
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    const auto hard_limit = stats.nodes > (max_size - stats.words_before) / 2
                                    ? max_size
                                    : stats.words_before + 2 * stats.nodes;
    const bool function_scope = input.ScopeKind() == Scope::Function;
    const bool signed_mul_widen_low = input.Target().Has(TargetFeature::VectorSignedMulWidenLow);
    const bool fault_locations = input.Target().Has(TargetFeature::FaultLocations);
    auto editor = Builder::Edit(std::move(input), std::min(hard_limit, max_operand_words));
    try {
        passes::Context context{
                editor, rows, stats, policy, signed_mul_widen_low, fault_locations};
        passes::FlagGraph flags;
        flags.Prepare(editor.Blocks(), function_scope, stats);
        auto error = passes::AnalyzeLiveness(context, flags);
        if (error == Error::None) error = passes::RewriteValues(context);
        if (error == Error::None) {
            if (context.needs_final_cleanup)
                error = passes::EliminateDeadCode(context, flags);
            else
                stats.final_walk_skipped = 1;
        }
        return Finish(editor, error, stats);
    } catch (const std::bad_alloc&) {
        return Finish(editor, Error::CapacityOverflow, stats, true);
    }
}

}  // namespace swift::runtime::ir
