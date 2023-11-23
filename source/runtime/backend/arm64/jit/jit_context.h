//
// Created by 甘尧 on 2023/9/27.
//

#pragma once

#include "runtime/backend/code_cache.h"
#include "runtime/common/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/ir/location.h"
#include "runtime/ir/instr.h"
#include "runtime/include/config.h"
#include "aarch64/macro-assembler-aarch64.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

class JitContext : DeleteCopyAndMove {
public:
    explicit JitContext(const Config& config, RegAlloc &reg_alloc);

    Register X(const ir::Value &value);
    VRegister V(const ir::Value &value);

    void Forward(ir::Location location);
    void Forward(const Register &location);
    void Finish();
    u8 *Flush(CodeCache &code_cache);

    MacroAssembler &GetMasm();

private:
    const Config &config;
    RegAlloc &reg_alloc;
    MacroAssembler masm;
};

}  // namespace swift::runtime::backend::arm64