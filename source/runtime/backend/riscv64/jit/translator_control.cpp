#include "translator.h"
#include "runtime/backend/riscv64/link.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/guarded_return_stack.h"
#include "runtime/backend/translate_table.h"

namespace swift::runtime::backend::riscv64 {
using namespace biscuit;
using O = ir::OpCode;

bool JitTranslator::EmitControl(ir::Inst* inst) {
    // Pop is performed by the terminal after all architectural effects.
    // CallReturn is a frontend continuation marker, paired with PushRSB.
    if (inst->GetOp() != O::PushRSB || !context.RSBEnabled()) return true;
    auto& as = context.GetMasm();
    const auto lambda = inst->GetArg<ir::Lambda>(0);
    if (lambda.IsValue()) return true;
    const auto guest = lambda.GetImm().Get();
    const auto index = context.DispatchIndex(guest);
    Label done, available, reset;
    context.Load(t0, state, state_offset_rsb_pointer);
    context.Load(t1, state, offsetof(State, rsb_empty));
    as.BEQ(t1, x0, &done);
    as.BLTU(t1, t0, &reset);
    // Explicit bounds keep overflow recovery out of a host signal frame.
    as.LI(t2, GuardedReturnStack::kUsableSize / 2);
    as.SUB(t2, t1, t2);
    as.BLTU(t2, t0, &available);
    as.Bind(&reset);
    as.MV(t0, t1);
    as.Bind(&available); as.ADDI(t0, t0, -s32(sizeof(RSBFrame)));
    as.LI(t2, guest); as.SD(t2, 0, t0);
    as.LI(t2, index); as.SD(t2, 8, t0);
    context.Store(t0, state, state_offset_rsb_pointer);
    as.Bind(&done); return true;
}

bool JitTranslator::PopRSB() {
    if (!context.RSBEnabled()) return false;
    auto& as = context.GetMasm();
    context.PublishUniformBindingsForExit();
    tail_used = true;
    Label miss, mismatch;
    context.Load(t0, state, state_offset_rsb_pointer);
    context.Load(t1, state, offsetof(State, rsb_empty));
    as.BEQ(t1, x0, &miss);
    as.BGEU(t0, t1, &mismatch);
    as.LI(t2, GuardedReturnStack::kUsableSize / 2); as.SUB(t2, t1, t2);
    as.BLTU(t0, t2, &mismatch);
    as.LD(t2, 0, t0); context.Load(t3, state, state_offset_current_loc);
    as.BNE(t2, t3, &mismatch);
    as.LD(t2, 8, t0); as.ADDI(t0, t0, sizeof(RSBFrame));
    context.Store(t0, state, state_offset_rsb_pointer);
    // GetDispatchIndex returns the value word index (2 * entry + 1).
    as.LI(t0, u64{1} << (HASH_TABLE_PAGE_BITS + 1)); as.BGEU(t2, t0, &mismatch);
    as.ANDI(t0, t2, 1); as.BEQ(t0, x0, &mismatch);
    context.Load(t0, state, state_offset_l2_code_cache);
    as.SLLI(t2, t2, 3); as.ADD(t0, t0, t2); as.LD(t3, 0, t0);
    as.FENCE(FenceOrder::R, FenceOrder::RW);
    context.BranchZero(t3, tail_epilogue, false);
    as.J(&miss);
    as.Bind(&mismatch); context.Store(t1, state, state_offset_rsb_pointer);
    as.Bind(&miss); context.Jump(epilogue); return true;
}

void JitTranslator::Link(u64 guest) {
    if (!context.DirectLinksEnabled()) { Return(HaltReason::None); return; }
    context.PublishUniformBindingsForExit();
    tail_used = true;
    auto& site = tail_sites.emplace_back(); site.guest = guest;
    context.GetMasm().LILabel(t3, &site.label);
    context.Jump(tail_epilogue);
}

void JitTranslator::EmitTailCode(u32 saved_mask) {
    if (!tail_used) return;
    auto& as = context.GetMasm();
    as.Bind(&tail_epilogue);
    context.Load(t0, state, state_offset_halt_reason, 4);
    context.BranchZero(t0, epilogue, false);
    context.ReleaseMemoryLease(); context.LeaveFloatMode();
    if (context.FlagsEnabled()) context.Store(flags, state, state_offset_host_flags);
    as.MV(a0, state);
    context.Store(x0, state, state_offset_riscv_recovery_pc);
    context.Store(x0, state, state_offset_riscv_recovery_frame);
    as.MV(sp, frame);
    as.LD(ra, 0, sp); as.LD(frame, 8, sp); as.LD(state, 16, sp); as.LD(values, 24, sp);
    for (u32 i = 0; i < scalar_registers.size(); ++i)
        if (saved_mask & (1u << i)) as.LD(scalar_registers[i], 32 + i * 8, sp);
    if (context.CallsABI() && context.EagerABISave())
        for (u32 i = 0; i < saved_fprs.size(); ++i) as.FLD(saved_fprs[i], 104 + i * 8, sp);
    as.ADDI(sp, sp, stats.frame_size); as.JR(t3);
    if (tail_sites.empty()) return;
    for (auto& site : tail_sites) {
        context.EnsureSpace(); as.Bind(&site.label);
        const auto offset = context.CurrentBufferSize();
        as.JAL(t3, &link_cold);
        context.LinkSites().push_back({.code_offset = offset, .guest_target = site.guest,
                                      .kind = u8(LinkSiteKind::Unconditional)});
        // Far links patch only the JAL to land here. The table slot is stable
        // across SMC; its value is acquired anew on every traversal.
        const auto index = context.DispatchIndex(site.guest);
        context.Load(t0, a0, state_offset_l2_code_cache);
        as.LI(t1, u64(index) * sizeof(u64)); as.ADD(t0, t0, t1);
        as.LD(t0, 0, t0); as.FENCE(FenceOrder::R, FenceOrder::RW);
        Label missing; as.BEQ(t0, x0, &missing); as.JR(t0);
        as.Bind(&missing); as.MV(a0, x0); as.RET();
    }
    as.Bind(&link_cold);
    as.ADDI(sp, sp, -32); as.SD(ra, 0, sp); as.SD(a0, 8, sp);
    as.ADDI(a1, t3, -4);
    context.HostAddress(t0, reinterpret_cast<u64>(&ResolveDirectLink)); as.JALR(t0);
    as.MV(t0, a0); as.LD(a0, 8, sp); as.LD(ra, 0, sp); as.ADDI(sp, sp, 32);
    Label missing; as.BEQ(t0, x0, &missing); as.JR(t0);
    as.Bind(&missing); as.MV(a0, x0); as.RET();
    for (auto& site : context.LinkSites()) if (!site.unlinked_instruction)
        std::memcpy(&site.unlinked_instruction, as.GetCodeBuffer().GetOffsetPointer(site.code_offset), 4);
}
} // namespace swift::runtime::backend::riscv64
