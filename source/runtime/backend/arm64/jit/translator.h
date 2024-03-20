#pragma once

#include "base/common_funcs.h"
#include "jit_context.h"
#include "runtime/backend/code_cache.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

class JitTranslator {
public:
    explicit JitTranslator(JitContext& ctx);

    void Translate();

    Operand EmitOperand(ir::Operand &ir_op);

    MemOperand EmitMemOperand(ir::Operand &ir_op);

#define INST(name, ...) void Emit##name(ir::Inst *inst);
#include "runtime/ir/ir.inc"
#undef INST

private:

    void Translate(ir::Inst *inst);

    JitContext &context;
    MacroAssembler &masm;
};

}