//
// Created by 甘尧 on 2023/9/27.
//

#pragma once

#include <map>
#include "aarch64/macro-assembler-aarch64.h"
#include "base/common_funcs.h"
#include "runtime/backend/address_space.h"
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
    explicit JitContext(const std::shared_ptr<Module> &module, RegAlloc& reg_alloc);

    [[nodiscard]] CPUReg Get(const ir::Value& value);
    [[nodiscard]] Register X(const ir::Value& value);
    [[nodiscard]] VRegister V(const ir::Value& value);

    [[nodiscard]] Register GetTmpX();
    [[nodiscard]] VRegister GetTmpV();

    void Forward(ir::Location location);
    void Forward(const Register& location);
    void Finish();
    [[nodiscard]] u32 CurrentBufferSize();
    u8* Flush(const CodeBuffer& code_cache);

    [[nodiscard]] MacroAssembler& GetMasm();

    void SetCurrent(ir::Function *function);
    void SetCurrent(ir::Block *block);
    void TickIR(ir::Inst* instr);

private:
    void BlockLinkStub(ir::Location location);

    vixl::aarch64::Label *GetLabel(LocationDescriptor loc);

    std::shared_ptr<Module> module;
    ir::Function *cur_function{};
    ir::Block *cur_block{};
    RegAlloc& reg_alloc;
    MacroAssembler masm;
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_gprs;
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_fprs;
    std::map<LocationDescriptor, Label> labels;
};

}  // namespace swift::runtime::backend::arm64