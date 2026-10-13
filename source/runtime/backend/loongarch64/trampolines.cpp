#include "trampolines.h"

#include <stdexcept>
#include "runtime/backend/context.h"
#include "runtime/backend/loongarch64/jit/jit_context.h"
#include "runtime/backend/translate_table.h"

namespace swift::runtime::backend::loongarch64 {
TrampolinesLoongArch64::TrampolinesLoongArch64(const Config& config) : Trampolines(config) {
    if (config.enable_asm_interp) throw std::invalid_argument("LoongArch cannot use the ARM64 assembly interpreter");
    JitContext context;
    auto& as = context.GetMasm();
    Label enter, dispatch, scan, found, code_miss, cache_miss, requested, done;
    as.addi_d(sp, sp, -s32(kSavedFrameSize));
    as.st_d(ra, sp, 0); as.st_d(frame, sp, 8); as.st_d(state, sp, 16); as.st_d(values, sp, 24);
    as.MV(frame, sp); as.MV(state, a0); as.MV(t0, a1); as.J(&enter);
    as.Bind(&dispatch);
    context.Load(t1, state, state_offset_current_loc);
    as.srli_d(t2, t1, 2); as.srli_d(t3, t2, HASH_TABLE_PAGE_BITS); as.xor_(t2, t2, t3);
    context.Mask(t2, HASH_TABLE_PAGE_BITS);
    as.LI(t4, u64{1} << HASH_TABLE_PAGE_BITS); as.sub_d(t4, t4, t2); as.slli_d(t2, t2, 4);
    context.Load(t3, state, state_offset_l2_code_cache); as.add_d(t3, t3, t2);
    as.Bind(&scan); as.BEQ(t4, zero, &code_miss);
    as.ld_d(t2, t3, 0); as.dbar(0);
    as.BEQ(t2, t1, &found); as.BEQ(t2, zero, &code_miss);
    as.addi_d(t3, t3, sizeof(TranslateEntry)); as.addi_d(t4, t4, -1); as.J(&scan);
    as.Bind(&found); as.ld_d(t0, t3, 8); as.BEQ(t0, zero, &cache_miss);
    as.Bind(&enter); context.Load(t1, state, state_offset_exit_request); as.dbar(0);
    as.BNE(t1, zero, &requested);
    as.MV(a0, state); as.jirl(ra, t0, 0); as.BNE(a0, zero, &done); as.J(&dispatch);
    as.Bind(&requested); as.srli_d(t1, t1, 63);
    as.LI(a0, static_cast<u32>(HaltReason::CodeMiss)); as.BEQ(t1, zero, &done);
    as.LI(a0, static_cast<u32>(HaltReason::Signal)); as.J(&done);
    as.Bind(&code_miss); as.LI(a0, static_cast<u32>(HaltReason::CodeMiss)); as.J(&done);
    as.Bind(&cache_miss); as.LI(a0, static_cast<u32>(HaltReason::CacheMiss)); as.J(&done);
    const auto return_offset = context.CurrentBufferSize();
    context.Load(a0, state, state_offset_halt_reason, 4);
    as.Bind(&done); context.Store(zero, state, state_offset_halt_reason, 4);
    as.MV(sp, frame); as.ld_d(ra, sp, 0); as.ld_d(frame, sp, 8);
    as.ld_d(state, sp, 16); as.ld_d(values, sp, 24); as.addi_d(sp, sp, kSavedFrameSize); as.jirl(zero, ra, 0);
    auto buffer = code_cache.AllocCode(context.CurrentBufferSize());
    if (!buffer) throw std::runtime_error("cannot allocate LoongArch dispatcher");
    context.Flush(*buffer);
    runtime_entry = reinterpret_cast<RuntimeEntry>(buffer->exec_data);
    return_host = reinterpret_cast<ReturnHost>(buffer->exec_data + return_offset);
    built = true;
}
}  // namespace swift::runtime::backend::loongarch64
