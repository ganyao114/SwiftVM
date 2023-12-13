//
// Created by 甘尧 on 2023/10/13.
//

#include "reg_alloc.h"

namespace swift::runtime::backend {

RegAlloc::RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs)
        : alloc_result{instr_size}, gprs(gprs), fprs(fprs) {}

void RegAlloc::MapRegister(u32 id, ir::HostFPR fpr) {
    auto &map = alloc_result[id];
    map.type = FPR;
    map.slot = fpr.id;
}

void RegAlloc::MapRegister(u32 id, ir::HostGPR gpr) {
    auto &map = alloc_result[id];
    map.type = GPR;
    map.slot = gpr.id;
}

void RegAlloc::SetActiveRegs(swift::u32 id, GPRSMask& gprs, FPRSMask& fprs) {
    auto &map = alloc_result[id];
    map.dirty_gprs = gprs;
    map.dirty_fprs = fprs;
}

ir::HostFPR RegAlloc::ValueFPR(const ir::Value& value) {
    return ValueFPR(value.Id());
}

ir::HostGPR RegAlloc::ValueGPR(const ir::Value& value) {
    return ValueGPR(value.Id());
}

ir::HostGPR RegAlloc::ValueGPR(u32 id) { return ir::HostGPR{alloc_result[id].slot}; }

ir::HostFPR RegAlloc::ValueFPR(u32 id) { return ir::HostFPR{alloc_result[id].slot}; }

const GPRSMask& RegAlloc::GetGprs() const { return gprs; }

const FPRSMask& RegAlloc::GetFprs() const { return fprs; }

ir::HostGPR RegAlloc::GetTmpGPR() {
    
    return ir::HostGPR{1};
}

ir::HostFPR RegAlloc::GetTmpFPR() {
    return ir::HostFPR{1};
}

void RegAlloc::SetCurrent(ir::Inst *inst) {
    current_ir = inst;
}

}  // namespace swift::runtime::backend
