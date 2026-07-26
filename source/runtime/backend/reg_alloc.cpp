//
// Created by 甘尧 on 2023/10/13.
//

#include <algorithm>
#include "reg_alloc.h"

namespace swift::runtime::backend {

// See reg_alloc.h for what this table is and who enforces it.
//
// Only opcodes that exceed the default appear here. Every entry is a measured
// peak over the full corpus, cross-checked against the emitter source:
//
//  * The float arithmetic family and X87Op all reach eight GPRs through
//    JitTranslator::EmitVecFloatNaNFixup, which keeps eight logical values
//    (raw operands, raw result, mask, scratch, two NaN predicates, the chosen
//    lane) live simultaneously while it rebuilds x86 NaN propagation lane by
//    lane in general-purpose registers.
//  * VecFCvtFloatToInt open-codes the x86 "invalid conversion -> INT_MIN"
//    rule per lane and holds five GPRs across it.
//  * VecFCvtPacked and VecFMulAdd hold five vector temporaries.
ScratchNeed ScratchBudget(ir::OpCode op) {
    switch (op) {
        // --- eight GPRs: the vector NaN-fixup path -----------------------
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        case ir::OpCode::VecFDiv:
        case ir::OpCode::VecFAddScalar32:
        case ir::OpCode::VecFSubScalar32:
        case ir::OpCode::VecFMulScalar32:
        case ir::OpCode::VecFDivScalar32:
        case ir::OpCode::VecFAddScalar64:
        case ir::OpCode::VecFSubScalar64:
        case ir::OpCode::VecFMulScalar64:
        case ir::OpCode::VecFDivScalar64:
        case ir::OpCode::X87Op:
            return {8, kDefaultScratchFPR};
        // --- five GPRs ---------------------------------------------------
        case ir::OpCode::VecFCvtFloatToInt:
            return {5, kDefaultScratchFPR};
        // --- five FPRs ---------------------------------------------------
        case ir::OpCode::VecFCvtPacked:
        case ir::OpCode::VecFMulAdd:
            return {kDefaultScratchGPR, 5};
        default:
            return {kDefaultScratchGPR, kDefaultScratchFPR};
    }
}

RegAlloc::RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs)
        : alloc_result(instr_size), gprs(gprs), fprs(fprs) {}

void RegAlloc::ResetAllocations() {
    std::fill(alloc_result.begin(), alloc_result.end(), Map{});
    current_ir = nullptr;
}

void RegAlloc::MapRegister(u32 id, ir::HostFPR fpr) {
    auto& map = alloc_result[id];
    map.type = FPR;
    map.slot = fpr.id;
}

void RegAlloc::MapRegister(u32 id, ir::HostGPR gpr) {
    auto& map = alloc_result[id];
    map.type = GPR;
    map.slot = gpr.id;
}

void RegAlloc::MapMemSpill(u32 id, ir::SpillSlot slot) {
    auto& map = alloc_result[id];
    map.type = MEM;
    map.slot = slot.offset;
}

void RegAlloc::MapReference(u32 from, u32 to) {
    auto& map = alloc_result[to];
    map.type = REF;
    map.slot = from;
}

void RegAlloc::SetActiveRegs(swift::u32 id, GPRSMask& gprs, FPRSMask& fprs) {
    auto& map = alloc_result[id];
    map.dirty_gprs = gprs;
    map.dirty_fprs = fprs;
}

ir::HostFPR RegAlloc::ValueFPR(const ir::Value& value) { return ValueFPR(value.Id()); }

ir::HostGPR RegAlloc::ValueGPR(const ir::Value& value) {
    return ValueGPR(value.Id());
}

ir::SpillSlot RegAlloc::ValueMem(const ir::Value& value) { return ValueMem(value.Id()); }

ir::HostGPR RegAlloc::ValueGPR(u32 id) {
    id = ResolveId(id);
    ASSERT(alloc_result[id].type == GPR);
    return ir::HostGPR{alloc_result[id].slot};
}

ir::HostFPR RegAlloc::ValueFPR(u32 id) {
    id = ResolveId(id);
    ASSERT(alloc_result[id].type == FPR);
    return ir::HostFPR{alloc_result[id].slot};
}

ir::SpillSlot RegAlloc::ValueMem(u32 id) {
    id = ResolveId(id);
    ASSERT(alloc_result[id].type == MEM);
    return ir::SpillSlot{alloc_result[id].slot};
}

RegAlloc::Type RegAlloc::ValueType(const ir::Value& value) {
    return alloc_result[ResolveId(value.Id())].type;
}

u32 RegAlloc::ResolveId(u32 id) const {
    while (alloc_result[id].type == REF) {
        id = alloc_result[id].slot;
    }
    return id;
}

const GPRSMask& RegAlloc::GetGprs() const { return gprs; }

const FPRSMask& RegAlloc::GetFprs() const { return fprs; }

GPRSMask RegAlloc::GetDirtyGPR() const {
    return alloc_result[current_ir->Id()].dirty_gprs;
}

FPRSMask RegAlloc::GetDirtyFPR() const {
    return alloc_result[current_ir->Id()].dirty_fprs;
}

void RegAlloc::SetCurrent(ir::Inst* inst) { current_ir = inst; }

}  // namespace swift::runtime::backend
