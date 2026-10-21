#include "translator.h"

namespace swift::runtime::backend::arm64 {

ir::Value JitTranslator::ResolveNarrowFlagsInput(ir::Value value,
                                                 ir::Inst* consumer) {
    if (!value.Def()) {
        return value;
    }
    auto candidate = IntegerFlagPreparation().inputs.find(value.Def());
    if (candidate == IntegerFlagPreparation().inputs.end()) {
        return value;
    }
    ASSERT_MSG(candidate->second.consumer == consumer,
               "narrow flags input consumer diverged at IR {}", consumer->Id());
    return candidate->second.source;
}

}  // namespace swift::runtime::backend::arm64
