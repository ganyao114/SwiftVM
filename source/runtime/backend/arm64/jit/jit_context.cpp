//
// Created by 甘尧 on 2023/9/15.
//

#include "jit_context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

JitContext::JitContext(const Config& config, RegAlloc &reg_alloc) : config(config), reg_alloc(reg_alloc) {}

CPUReg JitContext::Get(const ir::Value& value) {
    if (reg_alloc.ValueType(value) == RegAlloc::GPR) {
        return X(value);
    } else if (reg_alloc.ValueType(value) == RegAlloc::FPR) {
        return V(value);
    } else if (reg_alloc.ValueType(value) == RegAlloc::MEM) {
        ASSERT_MSG(false, "");
    } else {
        ASSERT_MSG(false, "");
    }
}

Register JitContext::X(const ir::Value& value) {
    auto reg = reg_alloc.ValueGPR(value);
    return XRegister::GetXRegFromCode(reg.id);
}

VRegister JitContext::V(const ir::Value& value) {
    auto reg = reg_alloc.ValueFPR(value);
    return VRegister::GetVRegFromCode(reg.id);
}

Register JitContext::GetTmpX() {
    auto reg = reg_alloc.GetTmpGPR();
    return XRegister::GetXRegFromCode(reg.id);
}

VRegister JitContext::GetTmpV() {
    auto reg = reg_alloc.GetTmpFPR();
    return VRegister::GetVRegFromCode(reg.id);
}

void JitContext::Forward(ir::Location location) {

}

void JitContext::Forward(const Register& location) {

}

void JitContext::Finish() {
    __ FinalizeCode();
}

u8* JitContext::Flush(CodeCache& code_cache) {
    auto cache_size = masm.GetBuffer()->GetSizeInBytes();
    auto buffer = code_cache.AllocCode(cache_size);
    if (buffer.has_value()) {
        std::memcpy(buffer->rw_data, masm.GetBuffer()->GetStartAddress<u8*>(), cache_size);
        buffer->Flush();
        return buffer->exec_data;
    } else {
        return nullptr;
    }
}

void JitContext::TickIR(ir::Inst* instr) {
    reg_alloc.SetCurrent(instr);
}

MacroAssembler& JitContext::GetMasm() {
    return masm;
}

#undef masm

}
