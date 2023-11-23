#pragma once

#include "runtime/backend/code_cache.h"
#include "runtime/common/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"
#include "jit_context.h"

namespace swift::runtime::backend::arm64 {

class JitTranslator {
public:
    explicit JitTranslator(JitContext& ctx);

    void Translate(ir::Inst *inst);

    Operand EmitOperand(ir::Operand &ir_op);

    MemOperand EmitMemOperand(ir::Operand &ir_op);

#define INST(name, ...) void Emit##name(ir::Inst *inst);
#include "runtime/ir/ir.inc"
#undef INST

private:
    JitContext &context;
    MacroAssembler &masm;
};

}