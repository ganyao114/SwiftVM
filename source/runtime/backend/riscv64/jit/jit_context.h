//
// Created by 甘尧 on 2023/12/13.
//

#pragma once

#include "base/common_funcs.h"
#include "runtime/backend/code_cache.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/location.h"
#include "assembler_riscv64.h"

namespace swift::runtime::backend::riscv64 {

using namespace swift::riscv64;

class JitContext : DeleteCopyAndMove {
public:
    explicit JitContext(const Config& config, RegAlloc &reg_alloc);

    XRegister X(const ir::Value &value);
    FRegister V(const ir::Value &value);

    XRegister GetTmpX();
    FRegister GetTmpV();

    void Forward(ir::Location location);
    void Forward(const XRegister &location);
    void Finish();
    u8 *Flush(CodeCache &code_cache);

    Riscv64Assembler &GetMasm();

    void TickIR(ir::Inst *instr);

private:
    const Config &config;
    RegAlloc &reg_alloc;
    ArenaAllocator masm_alloc{};
    Riscv64Assembler masm{&masm_alloc};
};

}
