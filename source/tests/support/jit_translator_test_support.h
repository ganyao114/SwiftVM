#pragma once
#include "runtime/backend/arm64/jit/translator.h"

namespace swift::runtime::backend::arm64 {
class JitTranslatorTestSupport {
public:
    static bool IsInstructionSuppressed(const JitTranslator& translator, const ir::Inst* inst) {
        return inst && translator.InstructionDisabled(inst->Id());
    }
    // Isolated encoders consume the same frozen actions as whole-block emission.
    static void BeginBlock(JitTranslator& translator, const ir::Block& block) {
        ASSERT(translator.ImportUnitEdges(&block, nullptr));
        translator.ImportBlockActions(&block);
        translator.cur_block = &block;
        translator.context.SetCurrent(&block);
    }
};
}
