#include "jit_context.h"

#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::riscv64 {
using namespace biscuit;

void JitContext::AcquireMemoryLease() {
    if (!memory_lease) return;
    // Match the cache's existing scratch convention: t0..t3 and a0..a7 may
    // contain a just-returned helper result or a pending guest operand.
    Label retry, anonymous, admitted, busy;
    HostAddress(t4, reinterpret_cast<u64>(&unaligned_atomic_lock));
    masm.Bind(&retry);
    Load(t5, state, state_offset_riscv_memory_participant);
    masm.LI(t6, 1);
    masm.BEQ(t5, x0, &anonymous);
    masm.SW(t6, 0, t5);
    masm.FENCE(FenceOrder::RW, FenceOrder::RW);
    masm.LWU(t6, 0, t4);
    masm.BEQ(t6, x0, &admitted);
    masm.SW(x0, 0, t5); masm.J(&busy);
    masm.Bind(&anonymous);
    HostAddress(t5, reinterpret_cast<u64>(&anonymous_memory_readers));
    masm.AMOADD_W(Ordering::AQRL, x0, t6, t5);
    masm.FENCE(FenceOrder::RW, FenceOrder::RW);
    masm.LWU(t6, 0, t4);
    masm.BEQ(t6, x0, &admitted);
    masm.LI(t6, u64{0} - 1); masm.AMOADD_W(Ordering::AQRL, x0, t6, t5);
    masm.Bind(&busy);
    masm.LWU(t6, 0, t4); masm.BNE(t6, x0, &busy); masm.J(&retry);
    masm.Bind(&admitted);
    // Observe a writer's release before subsequent guest reads/writes. The
    // earlier publication fence closes the scan race, but cannot acquire a
    // gate value read after that fence.
    masm.FENCE(FenceOrder::R, FenceOrder::RW);
    masm.LI(t6, 1);
    static_assert(state_offset_riscv_memory_owned <= 2047);
    masm.SD(t6, state_offset_riscv_memory_owned, state);
}

void JitContext::ReleaseMemoryLease() {
    if (!memory_lease) return;
    Label absent, anonymous, done;
    Load(t4, state, state_offset_riscv_memory_owned);
    masm.BEQ(t4, x0, &absent);
    masm.FENCE(FenceOrder::RW, FenceOrder::W);
    Load(t5, state, state_offset_riscv_memory_participant);
    masm.BEQ(t5, x0, &anonymous);
    masm.SW(x0, 0, t5); masm.J(&done);
    masm.Bind(&anonymous);
    HostAddress(t5, reinterpret_cast<u64>(&anonymous_memory_readers));
    masm.LI(t6, u64{0} - 1); masm.AMOADD_W(Ordering::RL, x0, t6, t5);
    masm.Bind(&done);
    Store(x0, state, state_offset_riscv_memory_owned);
    masm.Bind(&absent);
}

void JitContext::PollMemoryLease() {
    if (!memory_lease) return;
    Label resume;
    HostAddress(t4, reinterpret_cast<u64>(&unaligned_atomic_lock));
    masm.LWU(t5, 0, t4); masm.BEQ(t5, x0, &resume);
    ReleaseMemoryLease(); AcquireMemoryLease();
    masm.Bind(&resume);
}
} // namespace swift::runtime::backend::riscv64
