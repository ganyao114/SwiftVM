//
// Created by 甘尧 on 2023/9/27.
//

#pragma once

#include "aarch64/macro-assembler-aarch64.h"
#include "base/common_funcs.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/code_cache.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/location.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

using CPUReg = boost::variant<Register, VRegister>;

class JitContext : DeleteCopyAndMove {
public:
    explicit JitContext(const Config& config, RegAlloc &reg_alloc);

    CPUReg Get(const ir::Value &value);
    Register X(const ir::Value &value);
    VRegister V(const ir::Value &value);

    Register GetTmpX();
    VRegister GetTmpV();

    void Forward(ir::Location location);
    void Forward(const Register &location);
    void Finish();
    u8 *Flush(CodeCache &code_cache);

    MacroAssembler &GetMasm();

    void TickIR(ir::Inst *instr);

private:
    const Config &config;
    RegAlloc &reg_alloc;
    MacroAssembler masm;
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_gprs;
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_fprs;
};

}  // namespace swift::runtime::backend::arm64