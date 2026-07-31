#pragma once

#include <cstring>
#include "runtime/frontend/x86/decoder.h"

namespace swift::x86 {

// Lowering-only gates default on and accept =0 as an exact rollback.  Keep the
// getenv read uncached: the process-level JIT cache hash observes the same
// names, and test subprocesses can exercise either path independently.
inline bool VecLoweringEnabled(const char* name) {
    const char* env = swift::runtime::PerfGetenv(name);
    return !env || std::strcmp(env, "0") != 0;
}

// Opt-in vector lowerings use this while they are still under workload A/B.
// Unlike VecLoweringEnabled, an unset variable deliberately selects the old
// path so a freshly built binary is a true rollback baseline.
inline bool VecLoweringOptInEnabled(const char* name) {
    const char* env = swift::runtime::PerfGetenv(name);
    return env && std::strcmp(env, "0") != 0;
}

inline ir::Value VecSharedZero(ir::Assembler* assembler) {
    return assembler->VecSharedZero().SetType(ir::ValueType::V128);
}

inline ir::Value VecShuffle32Lowered(
        ir::Assembler* assembler, ir::Value source, u32 control) {
    if (!VecLoweringEnabled("SVM_VEC_CONST_CACHE")) {
        return assembler->VecShuffle32(source, ir::Imm(control))
                .SetType(ir::ValueType::V128);
    }
    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 output_lane = byte / 4;
        const u32 selected_lane = (control >> (output_lane * 2)) & 3;
        const u8 index = selected_lane * 4 + (byte & 3);
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }
    auto indexes = assembler->VecLoadConst(ir::Imm(index_lo), ir::Imm(index_hi))
                           .SetType(ir::ValueType::V128);
    return assembler->VecShuffle32Indexed(source, indexes)
            .SetType(ir::ValueType::V128);
}

inline ir::Value VecByteShiftLowered(
        ir::Assembler* assembler, ir::Value source, u32 count, bool left) {
    if (count == 0) {
        return source;
    }
    auto zero = VecSharedZero(assembler);
    if (count >= 16) {
        return zero;
    }
    return assembler->VecByteShift(
                            source, zero, ir::Imm(count), ir::Imm(left ? 1u : 0u))
            .SetType(ir::ValueType::V128);
}

u64 Paddb64(u64 a, u64 b);
u64 Psubb64(u64 a, u64 b);
u64 Paddw64(u64 a, u64 b);
u64 Psubw64(u64 a, u64 b);
u64 Paddd64(u64 a, u64 b);
u64 Psubd64(u64 a, u64 b);
u64 Pcmpeqb64(u64 a, u64 b);
u64 Pcmpeqw64(u64 a, u64 b);
u64 Pcmpeqd64(u64 a, u64 b);
u64 Pcmpgtb64(u64 a, u64 b);
u64 Pcmpgtw64(u64 a, u64 b);
u64 Pcmpgtd64(u64 a, u64 b);
u64 Pminub64(u64 a, u64 b);
u64 Pmaxub64(u64 a, u64 b);
u64 Pminud64(u64 a, u64 b);
u64 Pavgb64(u64 a, u64 b);
u64 Pavgw64(u64 a, u64 b);
u64 Psadbw64(u64 a, u64 b);
u64 PunpcklbwLo(u64 a, u64 b);
u64 PunpcklbwHi(u64 a, u64 b);
u64 PunpcklwdLo(u64 a, u64 b);
u64 PunpcklwdHi(u64 a, u64 b);
u64 Pmullw64(u64 a, u64 b);
u64 Pmaddwd64(u64 a, u64 b);
u64 Movshdup64(u64 a, u64 b);
u64 Movsldup64(u64 a, u64 b);
u64 ShufpsHalf(u64 lo, u64 hi, u64 imm_half);
u64 AddpsHalf(u64 a, u64 b);
u64 SubpsHalf(u64 a, u64 b);
u64 MulpsHalf(u64 a, u64 b);
u64 DivpsHalf(u64 a, u64 b);
u64 AddssHalf(u64 a, u64 b);
u64 SubssHalf(u64 a, u64 b);
u64 MulssHalf(u64 a, u64 b);
u64 DivssHalf(u64 a, u64 b);
u64 FxsaveFill(u64 guest_addr);

static ir::ValueType GetSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::U8;
        case 16:
            return ir::ValueType::U16;
        case 32:
            return ir::ValueType::U32;
        case 64:
            return ir::ValueType::U64;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

static ir::ValueType GetSignedSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::S8;
        case 16:
            return ir::ValueType::S16;
        case 32:
            return ir::ValueType::S32;
        case 64:
            return ir::ValueType::S64;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

// 64 bit signed values must be typed U64: the backend's context.R only
// promotes U64 to X registers, an S64 value would silently get a W register
// (32 bit truncation).
static ir::ValueType GetSignedContainer(u32 bits) {
    return bits == 64 ? ir::ValueType::U64 : GetSignedSize(bits);
}

static ir::ValueType GetVecSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::V8;
        case 16:
            return ir::ValueType::V16;
        case 32:
            return ir::ValueType::V32;
        case 64:
            return ir::ValueType::V64;
        case 128:
            return ir::ValueType::V128;
        case 256:
            return ir::ValueType::V256;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

}  // namespace swift::x86
