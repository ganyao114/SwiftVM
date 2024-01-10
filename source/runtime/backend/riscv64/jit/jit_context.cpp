//
// Created by 甘尧 on 2023/12/13.
//

#include "jit_context.h"

namespace swift::runtime::backend::riscv64 {

#define __ masm.

JitContext::JitContext(const Config& config, RegAlloc &reg_alloc) : config(config), reg_alloc(reg_alloc) {}

XRegister JitContext::X(const ir::Value& value) {
    auto reg = reg_alloc.ValueGPR(value);
    return XRegister(reg.id);
}

FRegister JitContext::V(const ir::Value& value) {
    auto reg = reg_alloc.ValueFPR(value);
    return FRegister(reg.id);
}

XRegister JitContext::GetTmpX() {
    auto reg = reg_alloc.GetTmpGPR();
    return XRegister(reg.id);
}

FRegister JitContext::GetTmpV() {
    auto reg = reg_alloc.GetTmpFPR();
    return FRegister(reg.id);
}

void JitContext::Forward(ir::Location location) {
    
}

void JitContext::Forward(const XRegister& location) {

}

void JitContext::Finish() {
    __ FinalizeCode();
}

u8* JitContext::Flush(CodeCache& code_cache) {
    auto cache_size = masm.GetBuffer()->Size();
    auto buffer = code_cache.AllocCode(cache_size);
    if (buffer.has_value()) {
        std::memcpy(buffer->rw_data, masm.GetBuffer()->contents(), cache_size);
        buffer->Flush();
        return buffer->exec_data;
    } else {
        return nullptr;
    }
}

void JitContext::TickIR(ir::Inst* instr) {
    reg_alloc.SetCurrent(instr);
}

Riscv64Assembler& JitContext::GetMasm() {
    return masm;
}

#undef masm

}
