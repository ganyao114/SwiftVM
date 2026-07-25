#pragma once

#include "runtime/frontend/x86/decoder.h"

namespace swift::x86 {

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
u64 AddpsHalf(u64 a, u64 b);
u64 SubpsHalf(u64 a, u64 b);
u64 MulpsHalf(u64 a, u64 b);
u64 DivpsHalf(u64 a, u64 b);
u64 AddssHalf(u64 a, u64 b);
u64 SubssHalf(u64 a, u64 b);
u64 MulssHalf(u64 a, u64 b);
u64 DivssHalf(u64 a, u64 b);
u64 Bswap64(u64 v, u64 width);
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
