#include <array>
#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

void X64Decoder::DecodeCpuid(_DInst& insn) {
    (void)insn;
    // Conservative feature set: SSE2 baseline plus CX16. AVX2 / AVX-512 /
    // BMI / ERMS are deliberately NOT reported so glibc's ifunc dispatch
    // selects the baseline SSE2 string routines (the EVEX implementations are
    // out of scope for this translator).
    static constexpr u32 kSse2Edx = (1u << 0)   // FPU
                                    | (1u << 15)  // CMOV
                                    | (1u << 23)  // MMX
                                    | (1u << 24)  // FXSR
                                    | (1u << 25)  // SSE
                                    | (1u << 26); // SSE2
    static constexpr u32 kLeaf1Ecx = (1u << 13); // CMPXCHG16B
    static constexpr u32 kExtEdx = (1u << 20)   // NX
                                   | (1u << 29);  // LM (required by 64 bit guests)
    auto leaf = __ ZeroExtend64(R(_RegisterType::R_EAX));
    auto is_leaf = [&](u32 n) {
        return __ TestZero(__ Xor(leaf, ir::Operand{ir::Imm(u64(n))}));
    };
    // Per-output-register leaf values {eax, ebx, ecx, edx}; unlisted leaves
    // and subleaves yield zeros.
    auto emit = [&](u32 for_leaf, std::array<u32, 4> vals) {
        auto cond = is_leaf(for_leaf);
        auto pick = [&](_RegisterType reg, u32 v) {
            R(reg, __ Select(cond, __ LoadImm(ir::Imm(u64(v))), R(reg))
                       .SetType(ir::ValueType::U32));
        };
        pick(_RegisterType::R_EAX, vals[0]);
        pick(_RegisterType::R_EBX, vals[1]);
        pick(_RegisterType::R_ECX, vals[2]);
        pick(_RegisterType::R_EDX, vals[3]);
    };
    // Start from zeros, then fold each supported leaf in.
    R(_RegisterType::R_EAX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_EBX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_ECX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_EDX, __ LoadImm(ir::Imm(u64(0))));
    emit(0x80000000, {0x80000004, 0, 0, 0});  // max extended leaf
    emit(0x80000001, {0, 0, 0, kExtEdx});
    emit(7, {0, 0, 0, 0});                     // no BMI / AVX2 / AVX-512 / ERMS
    emit(1, {0x000306C3, 0, kLeaf1Ecx, kSse2Edx}); // Haswell-ish model + CX16
    // "GenuineIntel" + max basic leaf.
    emit(0, {7, 0x756E6547, 0x6C65746E, 0x49656E69});
}

void X64Decoder::DecodeBswap(_DInst& insn) {
    // bswap r32/r64: reverse byte order. No flags affected.
    auto& op0 = insn.ops[0];
    u64 width = op0.size ? op0.size : 32;
    auto src = ToValue(Src(insn, op0));
    auto result = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&Bswap64)}},
                                src, __ LoadImm(ir::Imm(width)));
    Dst(insn, op0, result);
}

}  // namespace swift::x86
