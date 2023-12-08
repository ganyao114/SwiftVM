//
// Created by 甘尧 on 2023/10/13.
//

#include "reg_alloc.h"

namespace swift::runtime::backend {

RegAlloc::RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs)
        : alloc_result{instr_size}, gprs(gprs), fprs(fprs) {}

void RegAlloc::MapRegister(u32 id, ir::HostFPR fpr) {
    alloc_result[id].type = FPR;
    alloc_result[id].slot = fpr.id;
}

void RegAlloc::MapRegister(u32 id, ir::HostGPR gpr) {
    alloc_result[id].type = GPR;
    alloc_result[id].slot = gpr.id;
}

ir::HostFPR RegAlloc::ValueFPR(const ir::Value& value) { return {}; }

ir::HostGPR RegAlloc::ValueGPR(const ir::Value& value) { return {}; }

ir::HostGPR RegAlloc::ValueGPR(u32 id) { return ir::HostGPR{alloc_result[id].slot}; }

ir::HostFPR RegAlloc::ValueFPR(u32 id) { return ir::HostFPR{alloc_result[id].slot}; }

const GPRSMask& RegAlloc::GetGprs() const { return gprs; }

const FPRSMask& RegAlloc::GetFprs() const { return fprs; }

}  // namespace swift::runtime::backend
