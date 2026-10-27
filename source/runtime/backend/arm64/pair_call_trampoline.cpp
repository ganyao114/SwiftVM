#include "runtime/backend/arm64/pair_call_trampoline.h"

#include "runtime/common/helper_abi.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

void EmitPairCallTrampoline(MacroAssembler& assembler) {
    constexpr u32 gpr_frame_size = 20 * sizeof(u64);
#if SVM_HAS_HELPER_GENERAL_REGS_ONLY
    constexpr u32 frame_size = gpr_frame_size;
#else
    constexpr u32 frame_size = gpr_frame_size + 32 * sizeof(u128);
#endif
    assembler.Sub(sp, sp, frame_size);
    for (u32 code = 0; code < 18; code += 2) {
        assembler.Stp(XRegister(code),
                      XRegister(code + 1),
                      MemOperand(sp, code * sizeof(u64)));
    }
    assembler.Stp(x18, x30, MemOperand(sp, 18 * sizeof(u64)));
#if !SVM_HAS_HELPER_GENERAL_REGS_ONLY
    for (u32 code = 0; code < 32; code += 2) {
        assembler.Stp(VRegister::GetQRegFromCode(code),
                      VRegister::GetQRegFromCode(code + 1),
                      MemOperand(sp, gpr_frame_size + code * sizeof(u128)));
    }
#endif
    for (u32 index = 0; index < 3; ++index) {
        assembler.Ldr(XRegister(index),
                      MemOperand(sp, frame_size + PairCallFrame::Argument(index)));
    }
    assembler.Ldr(x16, MemOperand(sp, PairCallFrame::TargetRegister * sizeof(u64)));
    assembler.Blr(x16);
    assembler.Str(x0, MemOperand(sp, frame_size + PairCallFrame::PrimaryResult));
    assembler.Str(x1, MemOperand(sp, frame_size + PairCallFrame::SecondaryResult));
#if !SVM_HAS_HELPER_GENERAL_REGS_ONLY
    for (u32 code = 0; code < 32; code += 2) {
        assembler.Ldp(VRegister::GetQRegFromCode(code),
                      VRegister::GetQRegFromCode(code + 1),
                      MemOperand(sp, gpr_frame_size + code * sizeof(u128)));
    }
#endif
    for (u32 code = 0; code < 18; code += 2) {
        assembler.Ldp(XRegister(code),
                      XRegister(code + 1),
                      MemOperand(sp, code * sizeof(u64)));
    }
    assembler.Ldp(x18, x30, MemOperand(sp, 18 * sizeof(u64)));
    assembler.Add(sp, sp, frame_size);
    assembler.Ret();
}

}  // namespace swift::runtime::backend::arm64
