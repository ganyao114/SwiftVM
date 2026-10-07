#include "translator.h"

#include <cstring>
#include "runtime/backend/context.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

namespace {

bool CheckGuestRange(State* state, u64 guest, u64 size) noexcept {
    try {
        if (!state->interp_range_check(state->interp_range_check_ctx, guest, size))
            state->halt_reason = HaltReason::PageFatal;
    } catch (...) {
        state->halt_reason = HaltReason::IllegalCode;
    }
    return state->halt_reason == HaltReason::None;
}

}  // namespace

void JitTranslator::EmitAddress(u64 size) {
    // Input a1 is the guest effective address; output a0 is the host pointer.
    // Publish once before a potentially faulting access, including callbacks
    // that may observe State or abandon their own C++ callee-saved frame.
    ASSERT(size != 0);
    context.PublishFlags();
    auto& as = context.GetMasm();
    Label fault, no_oracle, done;
    context.Load(t0, state, offsetof(State, guest_addr_mask));
    as.AND(a1, a1, t0);
    context.Load(t1, state, offsetof(State, guest_addr_limit));
    as.BGEU(a1, t1, &fault);
    as.SUB(t1, t1, a1); as.LI(t2, size); as.BLTU(t1, t2, &fault);
    // For UINT64_MAX the bound check already rejects address wraparound;
    // this subtraction also handles every finite guest address mask.
    as.SUB(t0, t0, a1); as.LI(t2, size - 1); as.BLTU(t0, t2, &fault);
    context.Load(t0, state, offsetof(State, interp_range_check));
    as.BEQ(t0, x0, &no_oracle);
    context.SaveVectorsForCall();
    as.ADDI(sp, sp, -16); as.SD(a1, 0, sp);
    as.MV(a0, state); as.LI(a2, size);
    as.LI(t0, reinterpret_cast<u64>(&CheckGuestRange)); context.LeaveFloatMode(); as.JALR(t0); context.EnterFloatMode();
    context.ReloadFlags();
    context.RestoreVectorsAfterCall();
    as.LD(a1, 0, sp); as.ADDI(sp, sp, 16);
    context.BranchZero(a0, epilogue);
    as.Bind(&no_oracle);
    context.ResetVectorType();
    context.Load(t0, state, state_offset_pt);
    as.ADD(a0, a1, t0);
    as.J(&done);
    as.Bind(&fault);
    Return(HaltReason::PageFatal);
    as.Bind(&done);
}

bool JitTranslator::EmitMemoryCopy(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::MemoryCopy: case O::MemoryCopyTSO: break;
        default: return false;
    }
    const u64 size = inst->GetArg<ir::Imm>(2).Get();
    auto& as = context.GetMasm();
    const bool ordered = op == ir::OpCode::MemoryCopyTSO;
    if (ordered) as.FENCE(FenceOrder::RW, FenceOrder::RW);
    if (!size) { if (ordered) as.FENCE(FenceOrder::RW, FenceOrder::RW); return true; }
    const auto address = [&](const ir::Lambda& lambda) {
        if (lambda.IsValue()) context.Read(a1, lambda.GetValue());
        else as.LI(a1, lambda.GetImm().Get());
        EmitAddress(size);
    };
    address(inst->GetArg<ir::Lambda>(0));
    as.ADDI(sp, sp, -16); as.SD(a0, 0, sp);
    address(inst->GetArg<ir::Lambda>(1));
    as.MV(a5, a0); as.LD(a4, 0, sp); as.ADDI(sp, sp, 16);
    if (context.Features().vector) {
        if (size <= 16) {
            context.SetVectorType(8, u32(size)); as.VLE8(v24, a5); as.VSE8(v24, a4);
        } else {
            Label forward, forward_loop, backward_loop, done;
            as.LI(a2, size); as.BGEU(a5, a4, &forward);
            as.ADD(a4, a4, a2); as.ADD(a5, a5, a2);
            as.Bind(&backward_loop);
            as.VSETVLI(t2, a2, SEW::E8, LMUL::M8, VTA::No, VMA::No);
            as.SUB(a4, a4, t2); as.SUB(a5, a5, t2); as.VLE8(v24, a5); as.VSE8(v24, a4);
            as.SUB(a2, a2, t2); as.BNE(a2, x0, &backward_loop); as.J(&done);
            as.Bind(&forward); as.Bind(&forward_loop);
            as.VSETVLI(t2, a2, SEW::E8, LMUL::M8, VTA::No, VMA::No);
            as.VLE8(v24, a5); as.VSE8(v24, a4); as.ADD(a4, a4, t2); as.ADD(a5, a5, t2);
            as.SUB(a2, a2, t2); as.BNE(a2, x0, &forward_loop); as.Bind(&done);
            context.ResetVectorType();
        }
    } else if (size <= 16) {
        Label bytes, done;
        const u32 width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
        if (width > 1 && (size == width || size == 16)) {
            as.OR(t2, a4, a5); as.ANDI(t2, t2, width - 1); as.BNE(t2, x0, &bytes);
            context.Load(a2, a5, 0, width);
            if (size == 16) context.Load(a3, a5, 8, 8);
            context.Store(a2, a4, 0, width);
            if (size == 16) context.Store(a3, a4, 8, 8);
            as.J(&done);
            as.Bind(&bytes);
        }
        as.MV(a2, x0); as.MV(a3, x0);
        for (u32 offset = 0; offset < size; ++offset) {
            as.LBU(t2, offset, a5);
            if (offset % 8) as.SLLI(t2, t2, (offset % 8) * 8);
            as.OR(offset < 8 ? a2 : a3, offset < 8 ? a2 : a3, t2);
        }
        for (u32 offset = 0; offset < size; ++offset) {
            if (offset % 8) as.SRLI(t2, offset < 8 ? a2 : a3, (offset % 8) * 8);
            else as.MV(t2, offset < 8 ? a2 : a3);
            as.SB(t2, offset, a4);
        }
        as.Bind(&done);
    } else {
        context.SaveVectorsForCall();
        as.MV(a0, a4); as.MV(a1, a5); as.LI(a2, size); as.LI(t0, reinterpret_cast<u64>(&std::memmove));
        context.LeaveFloatMode(); as.JALR(t0); context.EnterFloatMode();
        context.ReloadFlags(); context.RestoreVectorsAfterCall();
    }
    if (ordered) as.FENCE(FenceOrder::RW, FenceOrder::RW);
    return true;
}

}  // namespace swift::runtime::backend::riscv64
