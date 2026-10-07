#include "trampolines.h"

#include <stdexcept>
#include "runtime/backend/context.h"
#include "runtime/backend/riscv64/defines.h"
#include "runtime/backend/riscv64/jit/jit_context.h"
#include "runtime/backend/translate_table.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

TrampolinesRiscv64::TrampolinesRiscv64(const Config& config) : Trampolines(config) {
    if (config.enable_asm_interp)
        throw std::invalid_argument("RV64 does not support the ARM64 assembly interpreter");
    // No static host pins: all guest architectural state is committed to State
    // at every block boundary and host call.
    JitContext context;
    auto& as = context.GetMasm();
    Label enter, dispatch, scan, found, code_miss, cache_miss, requested, done;
    as.ADDI(sp, sp, -static_cast<s32>(kSavedFrameSize));
    as.SD(ra, 0, sp); as.SD(frame, 8, sp);
    as.SD(state, 16, sp); as.SD(values, 24, sp);
    as.MV(frame, sp); as.MV(state, a0); as.MV(t0, a1);
    as.J(&enter);

    as.Bind(&dispatch);
    context.Load(t1, state, state_offset_current_loc);
    as.SRLI(t2, t1, 2);
    as.SRLI(t3, t2, HASH_TABLE_PAGE_BITS);
    as.XOR(t2, t2, t3);
    context.Mask(t2, HASH_TABLE_PAGE_BITS);
    as.LI(t4, u64{1} << HASH_TABLE_PAGE_BITS);
    as.SUB(t4, t4, t2);
    as.SLLI(t2, t2, 4);
    context.Load(t3, state, state_offset_l2_code_cache);
    as.ADD(t3, t3, t2);
    as.Bind(&scan);
    as.BEQ(t4, x0, &code_miss);
    as.LD(t2, 0, t3);
    as.FENCE(FenceOrder::R, FenceOrder::RW);
    as.BEQ(t2, t1, &found);
    as.BEQ(t2, x0, &code_miss);
    as.ADDI(t3, t3, sizeof(TranslateEntry)); as.ADDI(t4, t4, -1);
    as.J(&scan);
    as.Bind(&found);
    as.LD(t0, 8, t3);
    as.BEQ(t0, x0, &cache_miss);

    as.Bind(&enter);
    context.Load(t1, state, state_offset_exit_request);
    as.FENCE(FenceOrder::R, FenceOrder::RW);
    as.BNE(t1, x0, &requested);
    as.MV(a0, state);
    as.JALR(t0);
    as.BNE(a0, x0, &done);
    as.J(&dispatch);

    as.Bind(&requested);
    as.SRLI(t1, t1, 63);
    as.LI(a0, static_cast<u32>(HaltReason::CodeMiss));
    as.BEQ(t1, x0, &done);
    as.LI(a0, static_cast<u32>(HaltReason::Signal));
    as.J(&done);
    as.Bind(&code_miss);
    as.LI(a0, static_cast<u32>(HaltReason::CodeMiss)); as.J(&done);
    as.Bind(&cache_miss);
    as.LI(a0, static_cast<u32>(HaltReason::CacheMiss)); as.J(&done);
    const auto return_offset = context.CurrentBufferSize();
    context.Load(a0, state, state_offset_halt_reason, 4);
    as.Bind(&done);
    context.Store(x0, state, state_offset_halt_reason, 4);
    as.MV(sp, frame);
    as.LD(ra, 0, sp); as.LD(frame, 8, sp);
    as.LD(state, 16, sp); as.LD(values, 24, sp);
    as.ADDI(sp, sp, kSavedFrameSize); as.RET();

    const auto buffer = code_cache.AllocCode(context.CurrentBufferSize());
    if (!buffer) throw std::bad_alloc{};
    context.Flush(*buffer);
    runtime_entry = reinterpret_cast<RuntimeEntry>(buffer->exec_data);
    return_host = reinterpret_cast<ReturnHost>(buffer->exec_data + return_offset);
    built = true;
}

}  // namespace swift::runtime::backend::riscv64
