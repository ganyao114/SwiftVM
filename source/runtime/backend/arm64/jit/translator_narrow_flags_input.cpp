#include "translator.h"

namespace swift::runtime::backend::arm64 {

ir::Value JitTranslator::ResolveNarrowFlagsInput(ir::Value value, const ir::Inst* consumer) {
    if (!value.Def()) {
        return value;
    }
    const auto* candidate = context.GetActions().FlagsInput(value.Id());
    if (!candidate || !candidate->MatchesInput(*consumer, value)) return value;
    return candidate->Source(*consumer);
}

}  // namespace swift::runtime::backend::arm64
