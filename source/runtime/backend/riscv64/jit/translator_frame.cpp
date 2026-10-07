#include "translator.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

void JitTranslator::EmitPrologue(u32 entry_start) {
    // Emission tells us whether a C++ callee can be abandoned by recovery.
    // Only those blocks need the entire LP64D set; a leaf saves its actual
    // modified registers. Prepending shifts both endpoints of every already
    // resolved body-relative branch by the same amount.
    Assembler prefix;
    const auto saved_mask = context.CallsABI() ? (1u << scalar_registers.size()) - 1 : context.UsedGPRs();
    prefix.ADDI(sp, sp, -s32(stats.frame_size));
    prefix.SD(ra, 0, sp); prefix.SD(frame, 8, sp); prefix.SD(state, 16, sp); prefix.SD(values, 24, sp);
    for (u32 i = 0; i < scalar_registers.size(); ++i)
        if (saved_mask & (1u << i)) prefix.SD(scalar_registers[i], 32 + i * 8, sp);
    if (context.CallsABI()) for (u32 i = 0; i < saved_fprs.size(); ++i) prefix.FSD(saved_fprs[i], 104 + i * 8, sp);
    prefix.MV(frame, sp); prefix.MV(state, a0);
    if (context.VectorFloatEnabled()) {
        prefix.FRRM(t0); prefix.SD(t0, context.FloatModeOffset(), frame); prefix.FSRMI(0);
    }
    if (context.FlagsEnabled()) prefix.LD(flags, state_offset_host_flags, state);
    prefix.LI(t0, u64(slot_count) * kValueStride); prefix.SUB(sp, sp, t0); prefix.MV(values, sp);
    const auto recovery_source = prefix.GetCodeBuffer().GetCursorOffset();
    prefix.AUIPC(t0, 0); prefix.ADDI(t0, t0, 0);
    const auto store = [&](GPR source, u32 offset) {
        if (offset <= 2047) prefix.SD(source, offset, state);
        else { prefix.LI(t6, offset); prefix.ADD(t6, state, t6); prefix.SD(source, 0, t6); }
    };
    store(t0, state_offset_riscv_recovery_pc); store(frame, state_offset_riscv_recovery_frame);
    auto& buffer = prefix.GetCodeBuffer();
    const auto size = buffer.GetCursorOffset();
    const s64 recovery_delta = size + recovery_offset - entry_start - recovery_source;
    ASSERT(recovery_delta >= 0 && recovery_delta <= INT32_MAX);
    buffer.RewindCursor(recovery_source);
    prefix.AUIPC(t0, u32((recovery_delta + 0x800) >> 12) & 0xfffff);
    prefix.ADDI(t0, t0, s32(((recovery_delta & 0xfff) ^ 0x800) - 0x800));
    buffer.AdvanceCursor(size);
    context.Prepend(entry_start, {buffer.GetOffsetPointer(0), size_t(size)});
    recovery_offset += size;
}

}  // namespace swift::runtime::backend::riscv64
