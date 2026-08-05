#include <algorithm>
#include <cmath>
#include <cstring>
#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/fp_cmp_predicate.h"
#include "runtime/frontend/x86/x87.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

// SSE host helpers. Operations not represented in vector IR go through
// CallHost, mirroring the RepMovs / RepStos pattern: each helper computes ONE
// 64-bit half of a 128-bit lane vector, and the decoder invokes it twice.
// ---------------------------------------------------------------------------

u64 Paddb64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r |= u64(u8(u8(a >> (8 * i)) + u8(b >> (8 * i)))) << (8 * i);
    }
    return r;
}
u64 Psubb64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r |= u64(u8(u8(a >> (8 * i)) - u8(b >> (8 * i)))) << (8 * i);
    }
    return r;
}
u64 Paddw64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        r |= u64(u16(u16(a >> (16 * i)) + u16(b >> (16 * i)))) << (16 * i);
    }
    return r;
}
u64 Psubw64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        r |= u64(u16(u16(a >> (16 * i)) - u16(b >> (16 * i)))) << (16 * i);
    }
    return r;
}
u64 Paddd64(u64 a, u64 b) {
    u64 lo = u64(u32(u32(a) + u32(b)));
    u64 hi = u64(u32(u32(a >> 32) + u32(b >> 32)));
    return lo | (hi << 32);
}
u64 Psubd64(u64 a, u64 b) {
    u64 lo = u64(u32(u32(a) - u32(b)));
    u64 hi = u64(u32(u32(a >> 32) - u32(b >> 32)));
    return lo | (hi << 32);
}
u64 Pcmpeqb64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        if (u8(a >> (8 * i)) == u8(b >> (8 * i))) {
            r |= u64(0xFF) << (8 * i);
        }
    }
    return r;
}
u64 Pcmpeqw64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (u16(a >> (16 * i)) == u16(b >> (16 * i))) {
            r |= u64(0xFFFF) << (16 * i);
        }
    }
    return r;
}
u64 Pcmpeqd64(u64 a, u64 b) {
    u64 r = 0;
    if (u32(a) == u32(b)) {
        r |= 0xFFFFFFFFull;
    }
    if (u32(a >> 32) == u32(b >> 32)) {
        r |= 0xFFFFFFFFull << 32;
    }
    return r;
}
u64 Pcmpgtb64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        if (s8(a >> (8 * i)) > s8(b >> (8 * i))) {
            r |= u64(0xFF) << (8 * i);
        }
    }
    return r;
}
u64 Pcmpgtw64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (s16(a >> (16 * i)) > s16(b >> (16 * i))) {
            r |= u64(0xFFFF) << (16 * i);
        }
    }
    return r;
}
u64 Pcmpgtd64(u64 a, u64 b) {
    u64 r = 0;
    if (s32(a) > s32(b)) {
        r |= 0xFFFFFFFFull;
    }
    if (s32(a >> 32) > s32(b >> 32)) {
        r |= 0xFFFFFFFFull << 32;
    }
    return r;
}
u64 Pminub64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r |= u64(std::min(u8(a >> (8 * i)), u8(b >> (8 * i)))) << (8 * i);
    }
    return r;
}
u64 Pmaxub64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r |= u64(std::max(u8(a >> (8 * i)), u8(b >> (8 * i)))) << (8 * i);
    }
    return r;
}
u64 Pminud64(u64 a, u64 b) {
    u64 lo = std::min(u32(a), u32(b));
    u64 hi = std::min(u32(a >> 32), u32(b >> 32));
    return lo | (hi << 32);
}
u64 Pavgb64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r |= u64(u8((u16(u8(a >> (8 * i))) + u16(u8(b >> (8 * i))) + 1) >> 1)) << (8 * i);
    }
    return r;
}
u64 Pavgw64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        r |= u64(u16((u32(u16(a >> (16 * i))) + u32(u16(b >> (16 * i))) + 1) >> 1)) << (16 * i);
    }
    return r;
}
// psadbw: per-half the 8 byte |a-b| sums land in the low word.
u64 Psadbw64(u64 a, u64 b) {
    u64 sum = 0;
    for (u32 i = 0; i < 8; ++i) {
        int d = int(u8(a >> (8 * i))) - int(u8(b >> (8 * i)));
        sum += d < 0 ? -d : d;
    }
    return sum;
}
// punpcklbw/punpckhbw: interleave the low (high) bytes of each qword half.
u64 PunpcklbwLo(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        r |= u64(u8(a >> (8 * i))) << (16 * i);
        r |= u64(u8(b >> (8 * i))) << (16 * i + 8);
    }
    return r;
}
u64 PunpcklbwHi(u64 a, u64 b) { return PunpcklbwLo(a >> 32, b >> 32); }
u64 PunpcklwdLo(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 2; ++i) {
        r |= u64(u16(a >> (16 * i))) << (32 * i);
        r |= u64(u16(b >> (16 * i))) << (32 * i + 16);
    }
    return r;
}
u64 PunpcklwdHi(u64 a, u64 b) { return PunpcklwdLo(a >> 32, b >> 32); }
// pshufd: imm_half bit [8] selects the result half, [7:0] is the imm8.
u64 PshufdHalf(u64 lo, u64 hi, u64 imm_half) {
    u32 d[4] = {u32(lo), u32(lo >> 32), u32(hi), u32(hi >> 32)};
    u32 shift = (imm_half & 0x100) ? 4 : 0;
    u64 d0 = d[(imm_half >> shift) & 3];
    u64 d1 = d[(imm_half >> (shift + 2)) & 3];
    return d0 | (d1 << 32);
}
// shufps: lo half picks two dwords from operand a (imm bits [3:0]), hi half
// picks two dwords from operand b (imm bits [7:4]). imm_half bit [8] picks.
u64 ShufpsHalf(u64 lo, u64 hi, u64 imm_half) {
    u32 d[4] = {u32(lo), u32(lo >> 32), u32(hi), u32(hi >> 32)};
    u32 shift = (imm_half & 0x100) ? 4 : 0;
    u64 d0 = d[(imm_half >> shift) & 3];
    u64 d1 = d[(imm_half >> (shift + 2)) & 3];
    return d0 | (d1 << 32);
}
// Lane shifts. kind: 0 = word, 1 = dword, 2 = qword. Count follows the x86
// rule (saturating: count >= lane bits -> 0); callers pass the raw imm8 or
// the low qword of the count operand.
u64 Psll64(u64 v, u64 count, u64 kind) {
    switch (kind) {
        case 0: {
            if (count > 15) return 0;
            u64 r = 0;
            for (u32 i = 0; i < 4; ++i) {
                r |= (u64(u16(v >> (16 * i))) << count & 0xFFFF) << (16 * i);
            }
            return r;
        }
        case 1: {
            if (count > 31) return 0;
            u64 lo = (u64(u32(v)) << count) & 0xFFFFFFFFull;
            u64 hi = (u64(u32(v >> 32)) << count) & 0xFFFFFFFFull;
            return lo | (hi << 32);
        }
        default:
            if (count > 63) return 0;
            return v << count;
    }
}
u64 Psrl64(u64 v, u64 count, u64 kind) {
    switch (kind) {
        case 0: {
            if (count > 15) return 0;
            u64 r = 0;
            for (u32 i = 0; i < 4; ++i) {
                r |= u64(u16(v >> (16 * i)) >> count) << (16 * i);
            }
            return r;
        }
        case 1: {
            if (count > 31) return 0;
            u64 lo = u64(u32(v) >> count);
            u64 hi = u64(u32(v >> 32) >> count);
            return lo | (hi << 32);
        }
        default:
            if (count > 63) return 0;
            return v >> count;
    }
}
// psraw/psrad: arithmetic shift right, saturating. kind: 0=word, 1=dword.
u64 Psra64(u64 v, u64 count, u64 kind) {
    switch (kind) {
        case 0: {
            u64 c = std::min(count, u64(15));
            u64 r = 0;
            for (u32 i = 0; i < 4; ++i) {
                r |= u64(u16(s16(s16(v >> (16 * i)) >> c))) << (16 * i);
            }
            return r;
        }
        default: {
            u64 c = std::min(count, u64(31));
            u64 lo = u64(u32(s32(s32(u32(v)) >> c)));
            u64 hi = u64(u32(s32(s32(u32(v >> 32)) >> c)));
            return lo | (hi << 32);
        }
    }
}
// pmullw: signed word multiply, keep low 16 bits.
u64 Pmullw64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        r |= u64(u16(s16(a >> (16 * i)) * s16(b >> (16 * i)))) << (16 * i);
    }
    return r;
}
// pmaddwd: dst.d[i] = a.w[2i]*b.w[2i] + a.w[2i+1]*b.w[2i+1] (signed, per half).
u64 Pmaddwd64(u64 a, u64 b) {
    u64 r = 0;
    for (u32 i = 0; i < 2; ++i) {
        s32 p0 = s32(s16(a >> (32 * i))) * s32(s16(b >> (32 * i)));
        s32 p1 = s32(s16(a >> (32 * i + 16))) * s32(s16(b >> (32 * i + 16)));
        r |= u64(u32(p0 + p1)) << (32 * i);
    }
    return r;
}
// movshdup/movsldup: duplicate the odd/even dwords of the source half.
u64 Movshdup64(u64, u64 b) {
    u32 hi = u32(b >> 32);
    return u64(hi) | (u64(hi) << 32);
}
u64 Movsldup64(u64, u64 b) {
    u32 lo = u32(b);
    return u64(lo) | (u64(lo) << 32);
}
// haddps/hsubps used to live here as HaddpsHalf, a host-C lambda computing
// `f[0] - f[1]` / `f[2] - f[3]`.  It was REMOVED rather than fixed: at -O2 and
// above clang lowers that pair of subtractions to FNEG + FADDP, and FNEG flips
// the sign bit of a NaN, whereas x86 returns the operand's NaN quieted but
// otherwise unchanged.  Both instructions are now decoded by
// decoder_sse4.cc's pure-IR horizontal path (VecUnzip + VecFSub/VecFAdd, the
// same one vhaddps uses), whose VecFSub carries the x86 NaN-priority fixup.
// A dead implementation with a known-wrong answer is a mine, so nothing is
// left behind here.
//
// pextrw: extract word (imm & 7) from the 128-bit {lo, hi} source.
u64 Pextrw64(u64 lo, u64 hi, u64 imm) {
    u64 src = (imm & 4) ? hi : lo;
    return u64(u16(src >> (16 * (imm & 3))));
}
// pinsrw: insert the low word into lane (lane & 3) of a qword half.
u64 PinsrwHalf(u64 dst_half, u64 word, u64 lane) {
    u64 mask = ~(u64(0xFFFF) << (16 * (lane & 3)));
    return (dst_half & mask) | (u64(u16(word)) << (16 * (lane & 3)));
}
// addps/subps/mulps/divps: packed single-precision float, 2 lanes per u64 half.
#define DEFINE_PS_HALF(name, op)                                                                   \
    u64 name(u64 a, u64 b) {                                                                       \
        float fa[2], fb[2], fr[2];                                                                 \
        std::memcpy(fa, &a, 8);                                                                    \
        std::memcpy(fb, &b, 8);                                                                    \
        fr[0] = fa[0] op fb[0];                                                                    \
        fr[1] = fa[1] op fb[1];                                                                    \
        u64 r;                                                                                     \
        std::memcpy(&r, fr, 8);                                                                    \
        return r;                                                                                  \
    }
DEFINE_PS_HALF(AddpsHalf, +)
DEFINE_PS_HALF(SubpsHalf, -)
DEFINE_PS_HALF(MulpsHalf, *)
DEFINE_PS_HALF(DivpsHalf, /)
#undef DEFINE_PS_HALF
// addss/subss/mulss/divss: scalar single-precision float on the low dword;
// bits [63:32] of the low qword are preserved (high qword untouched by caller).
#define DEFINE_SS_HALF(name, op)                                                                   \
    u64 name(u64 a, u64 b) {                                                                       \
        float fa, fb, fr;                                                                          \
        u32 la = u32(a), lb = u32(b);                                                              \
        std::memcpy(&fa, &la, 4);                                                                  \
        std::memcpy(&fb, &lb, 4);                                                                  \
        fr = fa op fb;                                                                             \
        u32 r;                                                                                     \
        std::memcpy(&r, &fr, 4);                                                                   \
        return (a & 0xFFFFFFFF00000000ull) | u64(r);                                               \
    }
DEFINE_SS_HALF(AddssHalf, +)
DEFINE_SS_HALF(SubssHalf, -)
DEFINE_SS_HALF(MulssHalf, *)
DEFINE_SS_HALF(DivssHalf, /)
#undef DEFINE_SS_HALF
// pshuflw/pshufhw: word shuffle within a qword.
u64 Pshufw64(u64 v, u64 imm) {
    u64 r = 0;
    for (u32 i = 0; i < 4; ++i) {
        r |= u64(u16(v >> (16 * ((imm >> (2 * i)) & 3)))) << (16 * i);
    }
    return r;
}
// cvtsi2sd: signed int (width 32/64) to double.
u64 Cvtsi2sd64(u64 src, u64 width) {
    double d = width == 64 ? (double)(s64)src : (double)(s32)src;
    u64 r;
    std::memcpy(&r, &d, 8);
    return r;
}
// cvttsd2si: double to signed int (truncate). x86 "indefinite integer value"
// (INT_MIN) is returned for NaN and out-of-range inputs.
u64 Cvttsd2si64(u64 src, u64 width) {
    double d;
    std::memcpy(&d, &src, 8);
    if (width == 64) {
        if (std::isnan(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0)
            return 0x8000000000000000ull;
        return u64((s64)d);
    }
    if (std::isnan(d) || d >= 2147483648.0 || d < -2147483648.0) return u64(u32(0x80000000));
    return u64(u32((s32)d));
}
// cvtsd2ss: double to float (result in low 32 bits only; caller preserves [63:32]).
u64 Cvtsd2ss64(u64 src, u64) {
    double d;
    std::memcpy(&d, &src, 8);
    float f = (float)d;
    u32 r;
    std::memcpy(&r, &f, 4);
    return r;
}
// cvtss2sd: float (low 32 bits of src) to double.
u64 Cvtss2sd64(u64 src, u64) {
    float f;
    u32 lo = u32(src);
    std::memcpy(&f, &lo, 4);
    double d = (double)f;
    u64 r;
    std::memcpy(&r, &d, 8);
    return r;
}

u64 Pmovmskb(u64 lo, u64 hi) {
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r |= ((lo >> (8 * i + 7)) & 1) << i;
        r |= ((hi >> (8 * i + 7)) & 1) << (8 + i);
    }
    return r;
}
u64 Movmskps(u64 lo, u64 hi) {
    return ((lo >> 31) & 1) | (((lo >> 63) & 1) << 1) | (((hi >> 31) & 1) << 2) |
           (((hi >> 63) & 1) << 3);
}
u64 Movmskpd(u64 lo, u64 hi) { return ((lo >> 63) & 1) | (((hi >> 63) & 1) << 1); }
// pshufb: byte i of the ctrl half selects a byte from the full 128-bit a
// (high bit set -> 0).
u64 PshufbHalf(u64 a_lo, u64 a_hi, u64 ctrl) {
    u8 a[16];
    std::memcpy(a, &a_lo, 8);
    std::memcpy(a + 8, &a_hi, 8);
    u64 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        u8 c = u8(ctrl >> (8 * i));
        u8 v = (c & 0x80) ? 0 : a[c & 0x0F];
        r |= u64(v) << (8 * i);
    }
    return r;
}
// ucomisd: result bits mirror x86 flag semantics: bit0 = CF, bit1 = PF,
// bit2 = ZF (unordered sets all three).
u64 UcomisdFlags(u64 a, u64 b) {
    double da, db;
    std::memcpy(&da, &a, 8);
    std::memcpy(&db, &b, 8);
    if (std::isnan(da) || std::isnan(db)) {
        return 7;
    }
    if (da == db) {
        return 4;
    }
    if (da < db) {
        return 1;
    }
    return 0;
}

ir::Value X64Decoder::XmmRead(_RegisterType reg) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    return __ LoadUniform(ToVReg(x86_regs_table[reg]));
}

void X64Decoder::XmmWrite(_RegisterType reg, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    __ StoreUniform(ToVReg(x86_regs_table[reg]), value);
}

ir::Value X64Decoder::LoadSrcVec(_DInst& insn, _Operand& op) {
    if (op.type == O_REG) {
        return XmmRead(static_cast<_RegisterType>(op.index));
    }
    return __ LoadMemory(PlainStructuredAddress(insn, op)).SetType(ir::ValueType::V128);
}

ir::Value X64Decoder::XmmLo(_RegisterType reg) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    auto off = ToVReg(x86_regs_table[reg]).GetOffset();
    return __ LoadUniform(ir::Uniform{off, ir::ValueType::U64});
}

ir::Value X64Decoder::XmmScalarV(_RegisterType reg, u32 lane_bits) {
    ASSERT(lane_bits == 32 || lane_bits == 64);
    auto off = ToVReg(x86_regs_table[reg]).GetOffset();
    return __ LoadUniform(
            ir::Uniform{off,
                        lane_bits == 32 ? ir::ValueType::V32
                                        : ir::ValueType::V64});
}

ir::Value X64Decoder::XmmHi(_RegisterType reg) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    auto off = ToVReg(x86_regs_table[reg]).GetOffset();
    return __ LoadUniform(ir::Uniform{off + 8, ir::ValueType::U64});
}

void X64Decoder::XmmLo(_RegisterType reg, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    auto off = ToVReg(x86_regs_table[reg]).GetOffset();
    // NarrowTo normalizes untyped (CallLambda) values so the store has a width.
    __ StoreUniform(ir::Uniform{off, ir::ValueType::U64}, NarrowTo(value, ir::ValueType::U64));
}

void X64Decoder::XmmHi(_RegisterType reg, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    auto off = ToVReg(x86_regs_table[reg]).GetOffset();
    __ StoreUniform(ir::Uniform{off + 8, ir::ValueType::U64}, NarrowTo(value, ir::ValueType::U64));
}

ir::Value X64Decoder::FlatAddress(_DInst& insn, _Operand& op) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Address};
    auto address_operand = GetAddress(insn, op);
    // TSO / vector memory forms only encode [base]: fold the address into a
    // single value (same treatment as Src()).
    return __ GetOperand(address_operand.ToIROperand())
            .SetType(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
}

bool X64Decoder::CanStructureAddress(const _DInst& insn, const _Operand& op) const {
    if (!StructuredAddressModeEnabled()) {
        return false;
    }
    if ((SEGMENT_GET(insn.segment) != R_NONE) && !SEGMENT_IS_DEFAULT(insn.segment)) {
        return false;
    }
    switch (op.type) {
        case O_SMEM:
        case O_DISP:
            // One base/absolute term plus an optional displacement.
            return true;
        case O_MEM: {
            const bool has_base = insn.base != R_NONE;
            const bool has_index = op.index != R_NONE;
            const bool has_nonzero_disp = insn.dispSize && insn.disp != 0;
            // ir::Operand has left/right but no independent third data term.
            // Preserve only shapes it can carry without first materialising an
            // Add into SSA. Zero displacements are, of course, free.
            return !(has_base && has_index && has_nonzero_disp) &&
                   !(!has_base && has_index && insn.scale != 0 && has_nonzero_disp);
        }
        default:
            return false;
    }
}

ir::Operand X64Decoder::PlainStructuredAddress(_DInst& insn, _Operand& op) {
    if (!CanStructureAddress(insn, op)) {
        return ir::Operand{FlatAddress(insn, op)};
    }
    const bool was_building = building_structured_address;
    building_structured_address = true;
    auto address = GetAddress(insn, op).ToIROperand();
    building_structured_address = was_building;
    return address;
}

X64Decoder::VecHalves X64Decoder::LoadSrcHalves(_DInst& insn, _Operand& op) {
    if (op.type == O_REG) {
        auto reg = static_cast<_RegisterType>(op.index);
        return {XmmLo(reg), XmmHi(reg)};
    }
    auto addr = FlatAddress(insn, op);
    auto lo = __ LoadMemory(ir::Operand{addr}).SetType(ir::ValueType::U64);
    auto hi_addr = __ Add(addr, ir::Operand{ir::Imm(u64(8))});
    auto hi = __ LoadMemory(ir::Operand{hi_addr}).SetType(ir::ValueType::U64);
    return {lo, hi};
}

ir::Value X64Decoder::LoadSrcLo(_DInst& insn, _Operand& op) {
    if (op.type == O_REG) {
        return XmmLo(static_cast<_RegisterType>(op.index));
    }
    return __ LoadMemory(ir::Operand{FlatAddress(insn, op)}).SetType(ir::ValueType::U64);
}

ir::Value X64Decoder::LoadSrcScalarVec(_DInst& insn, _Operand& op, u32 lane_bits) {
    ASSERT(lane_bits == 32 || lane_bits == 64);
    if (op.type == O_REG) {
        return XmmScalarV(static_cast<_RegisterType>(op.index), lane_bits);
    }
    return __ LoadMemory(ir::Operand{FlatAddress(insn, op)})
            .SetType(lane_bits == 32 ? ir::ValueType::V32 : ir::ValueType::V64);
}

ir::Value X64Decoder::LoadSrcHi(_DInst& insn, _Operand& op) {
    if (op.type == O_REG) {
        return XmmHi(static_cast<_RegisterType>(op.index));
    }
    auto addr = __ Add(FlatAddress(insn, op), ir::Operand{ir::Imm(u64(8))});
    return __ LoadMemory(ir::Operand{addr}).SetType(ir::ValueType::U64);
}

void X64Decoder::DecodeVecHalfOp(_DInst& insn, VecHalfFn fn) { DecodeVecHalfOp(insn, fn, fn); }

void X64Decoder::DecodeVecHalfOp(_DInst& insn, VecHalfFn fn_lo, VecHalfFn fn_hi) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto a_lo = XmmLo(dst);
    auto a_hi = XmmHi(dst);
    auto b = LoadSrcHalves(insn, insn.ops[1]);
    // CallLambda directly: CallHost's FptrCast template cannot deduce from a
    // function pointer variable.
    XmmLo(dst, __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(fn_lo)}}, a_lo, b.lo));
    XmmHi(dst, __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(fn_hi)}}, a_hi, b.hi));
}

void X64Decoder::DecodePunpckLo(_DInst& insn, VecHalfFn fn_lo, VecHalfFn fn_hi) {
    // punpcklbw / punpcklwd: the full 128-bit result interleaves the LOW
    // qwords of both operands, so both helpers take (a_lo, b_lo).
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto a_lo = XmmLo(dst);
    auto b_lo = LoadSrcLo(insn, insn.ops[1]);
    XmmLo(dst, __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(fn_lo)}}, a_lo, b_lo));
    XmmHi(dst, __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(fn_hi)}}, a_lo, b_lo));
}

void X64Decoder::DecodeVec4Add(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result =
            __ Vec4Add(XmmRead(dst), LoadSrcVec(insn, insn.ops[1])).SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeVecBitwise(_DInst& insn, VecBitwiseOp op) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto right = LoadSrcVec(insn, insn.ops[1]);
    ir::Value result;
    switch (op) {
        case VecBitwiseOp::Xor:
            result = __ VecXor(left, right);
            break;
        case VecBitwiseOp::Or:
            result = __ VecOr(left, right);
            break;
        case VecBitwiseOp::And:
            result = __ VecAnd(left, right);
            break;
        case VecBitwiseOp::AndNot:
            // PANDN: dst = (NOT dst) AND src; VecAndNot(x, y) = x AND NOT y.
            result = __ VecAndNot(right, left);
            break;
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeVecInt(_DInst& insn, VecIntOp op, u32 lane_bits) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto right = LoadSrcVec(insn, insn.ops[1]);
    ir::Value result;
    switch (op) {
        case VecIntOp::Add:
            result = __ VecAdd(left, right, ir::Imm(lane_bits));
            break;
        case VecIntOp::Sub:
            result = __ VecSub(left, right, ir::Imm(lane_bits));
            break;
        case VecIntOp::CmpEq:
            result = __ VecCmpEq(left, right, ir::Imm(lane_bits));
            break;
        case VecIntOp::CmpGt:
            result = __ VecCmpGt(left, right, ir::Imm(lane_bits));
            break;
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeVecAvg(_DInst& insn, u32 lane_bits) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecAvg(XmmRead(dst), LoadSrcVec(insn, insn.ops[1]), ir::Imm(lane_bits))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeVecMinMax(_DInst& insn, bool max, u32 lane_bits, bool is_signed) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto right = LoadSrcVec(insn, insn.ops[1]);
    auto result = max ? __ VecMax(left, right, ir::Imm(lane_bits), ir::Imm(is_signed ? 1u : 0u))
                      : __ VecMin(left, right, ir::Imm(lane_bits), ir::Imm(is_signed ? 1u : 0u));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeVecMul(_DInst& insn, u32 lane_bits) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecMul(XmmRead(dst), LoadSrcVec(insn, insn.ops[1]), ir::Imm(lane_bits))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeVecMulHigh16(_DInst& insn, bool is_signed) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecMulHigh16(XmmRead(dst),
                                  LoadSrcVec(insn, insn.ops[1]),
                                  ir::Imm(is_signed ? 1u : 0u))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeVecSat(
        _DInst& insn, bool sub, u32 lane_bits, bool is_signed) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto right = LoadSrcVec(insn, insn.ops[1]);
    auto result = sub
            ? __ VecSatSub(left, right, ir::Imm(lane_bits), ir::Imm(u32(is_signed)))
            : __ VecSatAdd(left, right, ir::Imm(lane_bits), ir::Imm(u32(is_signed)));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeVecPack(
        _DInst& insn, u32 source_bits, bool unsigned_destination) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecPack(XmmRead(dst),
                             LoadSrcVec(insn, insn.ops[1]),
                             ir::Imm(source_bits),
                             ir::Imm(u32(unsigned_destination)));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeVecAbsDiffSum8(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecAbsDiffSum8(XmmRead(dst), LoadSrcVec(insn, insn.ops[1]))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeVecMadd16(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result =
            __ VecMadd16(XmmRead(dst), LoadSrcVec(insn, insn.ops[1])).SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeMaskmovdqu(_DInst& insn) {
    // MASKMOVDQU stores source bytes to the implicit [RDI] destination only
    // where the corresponding mask byte has its high bit set.  Keep each
    // store conditional: masked-off bytes must not introduce memory faults.
    const auto source = static_cast<_RegisterType>(insn.ops[0].index);
    const auto mask = static_cast<_RegisterType>(insn.ops[1].index);
    ir::Value source_halves[] = {XmmLo(source), XmmHi(source)};
    ir::Value mask_halves[] = {XmmLo(mask), XmmHi(mask)};
    auto base = R(_RegisterType::R_RDI);
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 half = byte / 8;
        const u32 shift = (byte % 8) * 8;
        auto shifted_source = shift == 0 ? source_halves[half]
                                         : __ LsrImm(source_halves[half], ir::Imm(shift));
        auto source_byte =
                __ And(shifted_source, ir::Operand{ir::Imm(u64(0xFF))})
                        .SetType(ir::ValueType::U8);
        auto mask_byte = shift == 0 ? mask_halves[half]
                                    : __ LsrImm(mask_halves[half], ir::Imm(shift));
        auto enabled = __ TestNotZero(
                __ And(mask_byte, ir::Operand{ir::Imm(u64(0x80))}));
        auto skip = __ NotGoto(enabled);
        // The architectural non-temporal hint is not observable in SwiftVM;
        // use the configured TSO store path, as MOVNTI does.
        MemStore(ir::Operand{base, static_cast<s32>(byte), ir::OperandPlus},
                 source_byte,
                 TsoOrdered(insn));
        __ BindLabel(skip);
    }
}

void X64Decoder::DecodeVecZip(_DInst& insn, u32 lane_bits, bool high) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecZip(XmmRead(dst),
                            LoadSrcVec(insn, insn.ops[1]),
                            ir::Imm(lane_bits),
                            ir::Imm(high ? 1u : 0u))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeVecDupPairs32(_DInst& insn, bool odd) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecDupPairs32(LoadSrcVec(insn, insn.ops[1]), ir::Imm(odd ? 1u : 0u))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeVecHalfOpHigh(_DInst& insn, VecHalfFn fn_lo, VecHalfFn fn_hi) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto a_hi = XmmHi(dst);
    auto b_hi = LoadSrcHi(insn, insn.ops[1]);
    XmmLo(dst, __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(fn_lo)}}, a_hi, b_hi));
    XmmHi(dst, __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(fn_hi)}}, a_hi, b_hi));
}

void X64Decoder::DecodeMuludq(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto a_lo = XmmLo(dst);
    auto a_hi = XmmHi(dst);
    auto b = LoadSrcHalves(insn, insn.ops[1]);
    const auto kLo32 = ir::Imm(u64(0xFFFFFFFF));
    auto lo =
            __ Mul(__ And(a_lo, ir::Operand{kLo32}), ir::Operand{__ And(b.lo, ir::Operand{kLo32})});
    auto hi =
            __ Mul(__ And(a_hi, ir::Operand{kLo32}), ir::Operand{__ And(b.hi, ir::Operand{kLo32})});
    XmmLo(dst, lo);
    XmmHi(dst, hi);
}

void X64Decoder::DecodeMovd(_DInst& insn) {
    swift::runtime::PerfLoweringHandlerBegin();
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    // REX.W forms (66 REX.W 0F 6E/7E) are 64-bit: alias to the movq path.
    if (op0.size == 64 || op1.size == 64) {
        DecodeMovq(insn);
        return;
    }
    if (op0.type == O_REG && IsV(static_cast<_RegisterType>(op0.index))) {
        // movd xmm, r/m32: low dword = src, upper 96 bits zeroed.
        auto dst = static_cast<_RegisterType>(op0.index);
        auto src = ToValue(Src(insn, op1));
        XmmLo(dst, __ ZeroExtend64(src));
        XmmHi(dst, __ LoadImm(ir::Imm(u64(0))));
    } else {
        // movd r/m32, xmm
        Dst(insn, op0, XmmLo(static_cast<_RegisterType>(op1.index)));
    }
}

void X64Decoder::DecodeMovq(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG && IsV(static_cast<_RegisterType>(op0.index))) {
        // movq xmm, xmm/r64/m64: low qword = src, high qword zeroed.
        auto dst = static_cast<_RegisterType>(op0.index);
        ir::Value v;
        if (op1.type == O_REG && IsV(static_cast<_RegisterType>(op1.index))) {
            v = XmmLo(static_cast<_RegisterType>(op1.index));
        } else {
            v = ToValue(Src(insn, op1));
        }
        XmmLo(dst, v);
        XmmHi(dst, __ LoadImm(ir::Imm(u64(0))));
    } else {
        // movq r64/m64, xmm
        Dst(insn, op0, XmmLo(static_cast<_RegisterType>(op1.index)));
    }
}

void X64Decoder::DecodeMovVec(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG) {
        // Load: xmm, xmm/m128 (movnt* only store, handled by the else branch).
        ir::Value v;
        if (op1.type == O_REG) {
            v = __ LoadUniform(ToVReg(x86_regs_table[op1.index]));
        } else {
            v = __ LoadMemory(PlainStructuredAddress(insn, op1))
                        .SetType(ir::ValueType::V128);
        }
        __ StoreUniform(ToVReg(x86_regs_table[op0.index]), v);
    } else {
        // Store: m128, xmm (movntdq/movntps degrade to plain stores).
        auto v = __ LoadUniform(ToVReg(x86_regs_table[op1.index]));
        __ StoreMemory(PlainStructuredAddress(insn, op0), v);
    }
}

void X64Decoder::DecodeMovsd(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG && IsV(static_cast<_RegisterType>(op0.index))) {
        auto dst = static_cast<_RegisterType>(op0.index);
        if (op1.type == O_REG) {
            // xmm, xmm: low qword copied, high qword preserved.
            XmmLo(dst, XmmLo(static_cast<_RegisterType>(op1.index)));
        } else {
            // xmm, m64: low qword loaded, high qword zeroed.
            auto v = __ LoadMemory(ir::Operand{FlatAddress(insn, op1)}).SetType(ir::ValueType::U64);
            XmmLo(dst, v);
            XmmHi(dst, __ LoadImm(ir::Imm(u64(0))));
        }
    } else {
        // m64 = src low qword.
        __ StoreMemory(ir::Operand{FlatAddress(insn, op0)},
                       XmmLo(static_cast<_RegisterType>(op1.index)));
    }
}

void X64Decoder::DecodeMovss(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG && IsV(static_cast<_RegisterType>(op0.index))) {
        auto dst = static_cast<_RegisterType>(op0.index);
        if (op1.type == O_REG) {
            // xmm, xmm: low dword copied, upper 96 bits preserved.
            auto merged = __ Or(__ And(XmmLo(dst), ir::Operand{ir::Imm(u64(0xFFFFFFFF00000000))}),
                                ir::Operand{__ And(XmmLo(static_cast<_RegisterType>(op1.index)),
                                                   ir::Operand{ir::Imm(u64(0xFFFFFFFF))})});
            XmmLo(dst, merged);
        } else {
            // xmm, m32: low dword loaded, upper 96 bits zeroed.
            auto v = __ LoadMemory(ir::Operand{FlatAddress(insn, op1)}).SetType(ir::ValueType::U32);
            XmmLo(dst, __ ZeroExtend64(v));
            XmmHi(dst, __ LoadImm(ir::Imm(u64(0))));
        }
    } else {
        // m32 = src low dword.
        Dst(insn, op0, XmmLo(static_cast<_RegisterType>(op1.index)));
    }
}

void X64Decoder::DecodeMovHalf(_DInst& insn, bool high) {
    swift::runtime::PerfLoweringHandlerBegin();
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG && IsV(static_cast<_RegisterType>(op0.index))) {
        // Load: xmm half, m64 (other half preserved).
        auto v = __ LoadMemory(ir::Operand{FlatAddress(insn, op1)}).SetType(ir::ValueType::U64);
        if (high) {
            XmmHi(static_cast<_RegisterType>(op0.index), v);
        } else {
            XmmLo(static_cast<_RegisterType>(op0.index), v);
        }
    } else {
        // Store: m64 = xmm half.
        auto half = high ? XmmHi(static_cast<_RegisterType>(op1.index))
                         : XmmLo(static_cast<_RegisterType>(op1.index));
        __ StoreMemory(ir::Operand{FlatAddress(insn, op0)}, half);
    }
}

void X64Decoder::DecodeMovhlps(_DInst& insn, bool low_to_high) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto src = static_cast<_RegisterType>(insn.ops[1].index);
    if (low_to_high) {
        // MOVLHPS: dst[127:64] = src[63:0]
        XmmHi(dst, XmmLo(src));
    } else {
        // MOVHLPS: dst[63:0] = src[127:64]
        XmmLo(dst, XmmHi(src));
    }
}

void X64Decoder::DecodeMovmsk(_DInst& insn, bool pd) {
    auto src = static_cast<_RegisterType>(insn.ops[1].index);
    auto mask = __ VecMovMask(XmmRead(src), ir::Imm(pd ? 64u : 32u)).SetType(ir::ValueType::U32);
    Dst(insn, insn.ops[0], mask);
}

void X64Decoder::DecodePshufd(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = VecShuffle32Lowered(
            assembler, LoadSrcVec(insn, insn.ops[1]), insn.imm.byte);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeShufps(_DInst& insn, bool pd) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    u64 imm = insn.imm.byte;
    // Default ON after the flip A/B (c-ray 5/5 pairs positive, median 1.44);
    // SVM_VEC_SHUFPS_NEON=0 selects the old helper path as the rollback.
    const bool shufps_neon = swift::runtime::GetSvmConfig().vec_shufps_neon;
    if (!pd && shufps_neon) {
        // Read the complete old destination before touching the source. Legacy
        // SHUFPS is destructive two-operand syntax even though the IR is
        // naturally three-operand.
        auto a = XmmRead(dst);
        ir::Value source;
        if (insn.ops[1].type == O_REG &&
            static_cast<_RegisterType>(insn.ops[1].index) == dst) {
            source = a;
        } else {
            source = LoadSrcVec(insn, insn.ops[1]);
        }
        auto result = __ VecShuffle32TwoSrc(a, source, ir::Imm(imm))
                              .SetType(ir::ValueType::V128);
        XmmWrite(dst, result);
        return;
    }
    auto a_lo = XmmLo(dst);
    auto a_hi = XmmHi(dst);
    auto b = LoadSrcHalves(insn, insn.ops[1]);
    if (pd) {
        // shufpd: dst.lo = (imm&1) ? a.hi : a.lo; dst.hi = (imm&2) ? b.hi : b.lo
        XmmLo(dst, (imm & 1) ? a_hi : a_lo);
        XmmHi(dst, (imm & 2) ? b.hi : b.lo);
        return;
    }
    auto lo = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&ShufpsHalf)}},
                            a_lo,
                            a_hi,
                            __ LoadImm(ir::Imm(imm)));
    auto hi = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&ShufpsHalf)}},
                            b.lo,
                            b.hi,
                            __ LoadImm(ir::Imm(imm | 0x100)));
    XmmLo(dst, lo);
    XmmHi(dst, hi);
}

void X64Decoder::DecodePshiftDQ(_DInst& insn, bool left) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    u64 imm = insn.imm.byte;
    if (VecLoweringEnabled(swift::runtime::GetSvmConfig().vec_byteshift_ext)) {
        // Legacy shift-by-zero is a true identity: avoiding XmmWrite also
        // preserves the old no-IR shape.  VEX.128 has a different upper-lane
        // contract and is handled separately in decoder_avx_int.cc.
        if (imm == 0) {
            return;
        }
        XmmWrite(dst, VecByteShiftLowered(assembler, XmmRead(dst), imm, left));
        return;
    }
    if (imm == 0) {
        return;  // identity
    }
    if (imm >= 16) {
        XmmLo(dst, __ LoadImm(ir::Imm(u64(0))));
        XmmHi(dst, __ LoadImm(ir::Imm(u64(0))));
        return;
    }
    auto a_lo = XmmLo(dst);
    auto a_hi = XmmHi(dst);
    u32 bits = u32(imm * 8);
    ir::Value lo, hi;
    if (left) {
        // 128-bit left shift (bytes move toward higher addresses).
        if (imm < 8) {
            lo = __ LslImm(a_lo, ir::Imm(u64(bits)));
            hi = __ Or(__ LslImm(a_hi, ir::Imm(u64(bits))),
                       ir::Operand{__ LsrImm(a_lo, ir::Imm(u64(64 - bits)))});
        } else if (imm == 8) {
            lo = __ LoadImm(ir::Imm(u64(0)));
            hi = a_lo;
        } else {
            lo = __ LoadImm(ir::Imm(u64(0)));
            hi = __ LslImm(a_lo, ir::Imm(u64(bits - 64)));
        }
    } else {
        if (imm < 8) {
            lo = __ Or(__ LsrImm(a_lo, ir::Imm(u64(bits))),
                       ir::Operand{__ LslImm(a_hi, ir::Imm(u64(64 - bits)))});
            hi = __ LsrImm(a_hi, ir::Imm(u64(bits)));
        } else if (imm == 8) {
            lo = a_hi;
            hi = __ LoadImm(ir::Imm(u64(0)));
        } else {
            lo = __ LsrImm(a_hi, ir::Imm(u64(bits - 64)));
            hi = __ LoadImm(ir::Imm(u64(0)));
        }
    }
    XmmLo(dst, lo);
    XmmHi(dst, hi);
}

void X64Decoder::DecodePshift(_DInst& insn, bool left, int kind) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto& op1 = insn.ops[1];
    const u32 lane_bits = 16u << kind;
    ir::Value result;
    if (op1.type == O_IMM) {
        if (VecLoweringEnabled(swift::runtime::GetSvmConfig().vec_imm_shift)) {
            result = left
                             ? __ VecShiftLeftImm(
                                       XmmRead(dst),
                                       ir::Imm(u64(insn.imm.byte)),
                                       ir::Imm(lane_bits))
                             : __ VecShiftRightImm(
                                       XmmRead(dst),
                                       ir::Imm(u64(insn.imm.byte)),
                                       ir::Imm(lane_bits));
        } else {
            auto count = __ LoadImm(ir::Imm(u64(insn.imm.byte)));
            result = left ? __ VecShiftLeft(XmmRead(dst), count, ir::Imm(lane_bits))
                          : __ VecShiftRight(XmmRead(dst), count, ir::Imm(lane_bits));
        }
    } else {
        // xmm/m128 count operand: the count is the low qword.
        auto count = LoadSrcLo(insn, op1);
        result = left ? __ VecShiftLeft(XmmRead(dst), count, ir::Imm(lane_bits))
                      : __ VecShiftRight(XmmRead(dst), count, ir::Imm(lane_bits));
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodePshiftA(_DInst& insn, int kind) {
    // psraw/psrad: arithmetic right shift, saturating.
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto& op1 = insn.ops[1];
    const u32 lane_bits = 16u << kind;
    ir::Value result;
    if (op1.type == O_IMM) {
        if (VecLoweringEnabled(swift::runtime::GetSvmConfig().vec_imm_shift)) {
            result = __ VecShiftRightArithmeticImm(
                    XmmRead(dst),
                    ir::Imm(u64(insn.imm.byte)),
                    ir::Imm(lane_bits));
        } else {
            auto count = __ LoadImm(ir::Imm(u64(insn.imm.byte)));
            result = __ VecShiftRightArithmetic(XmmRead(dst), count, ir::Imm(lane_bits));
        }
    } else {
        auto count = LoadSrcLo(insn, op1);
        result = __ VecShiftRightArithmetic(XmmRead(dst), count, ir::Imm(lane_bits));
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodePshufw(_DInst& insn, bool high) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result =
            __ VecShuffle16(
                    LoadSrcVec(insn, insn.ops[1]), ir::Imm(insn.imm.byte), ir::Imm(high ? 1u : 0u))
                    .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodePackedFloatOp(_DInst& insn, VecFloatOp op, u32 lane_bits) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto right = LoadSrcVec(insn, insn.ops[1]);
    ir::Value result;
    switch (op) {
        case VecFloatOp::Add:
            result = __ VecFAdd(left, right, ir::Imm(lane_bits));
            break;
        case VecFloatOp::Sub:
            result = __ VecFSub(left, right, ir::Imm(lane_bits));
            break;
        case VecFloatOp::Mul:
            result = __ VecFMul(left, right, ir::Imm(lane_bits));
            break;
        case VecFloatOp::Div:
            result = __ VecFDiv(left, right, ir::Imm(lane_bits));
            break;
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeCvtsi2ss(_DInst& insn) {
    // cvtsi2ss xmm, r/m32/64: only low dword changes; upper 96 bits are
    // preserved from the first (destination) operand.
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto& op1 = insn.ops[1];
    const u32 width = op1.size ? op1.size : 32;
    ir::Value src = op1.type == O_REG ? R(static_cast<_RegisterType>(op1.index))
                                      : MemLoad(ir::Operand{FlatAddress(insn, op1)},
                                                GetSize(width),
                                                false);
    auto converted =
            __ VecFCvtIntToFloat(src, ir::Imm(width), ir::Imm(32)).SetType(ir::ValueType::U32);
    auto merged = __ Or(__ And(XmmLo(dst), ir::Operand{ir::Imm(u64(0xFFFFFFFF00000000))}),
                        ir::Operand{__ And(converted, ir::Operand{ir::Imm(u64(0xFFFFFFFF))})});
    XmmLo(dst, merged);
}

void X64Decoder::DecodeCvtsi2sd(_DInst& insn) {
    // cvtsi2sd xmm, r/m32/64: dst[63:0] = (double)(int)src; high qword unchanged.
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto& op1 = insn.ops[1];
    const u32 width = op1.size ? op1.size : 32;
    ir::Value src = op1.type == O_REG ? R(static_cast<_RegisterType>(op1.index))
                                      : MemLoad(ir::Operand{FlatAddress(insn, op1)},
                                                GetSize(width),
                                                false);
    XmmLo(dst,
          __ VecFCvtIntToFloat(src, ir::Imm(width), ir::Imm(64)).SetType(ir::ValueType::U64));
}

void X64Decoder::DecodeCvttss2si(_DInst& insn) {
    auto& op0 = insn.ops[0];
    const u32 width = op0.size ? op0.size : 32;
    auto src = LoadSrcLo(insn, insn.ops[1]);
    auto result = __ VecFCvtFloatToInt(src, ir::Imm(32), ir::Imm(width), ir::Imm(0u));
    Dst(insn, op0, result.SetType(width == 64 ? ir::ValueType::U64 : ir::ValueType::U32));
}

void X64Decoder::DecodeCvttsd2si(_DInst& insn) {
    // cvttsd2si r32/64, xmm/m64: dst = truncate_to_int(src[63:0]).
    auto& op0 = insn.ops[0];
    const u32 width = op0.size ? op0.size : 32;
    auto src = LoadSrcLo(insn, insn.ops[1]);
    auto result = __ VecFCvtFloatToInt(src, ir::Imm(64), ir::Imm(width), ir::Imm(0u));
    Dst(insn, op0, result.SetType(width == 64 ? ir::ValueType::U64 : ir::ValueType::U32));
}

void X64Decoder::DecodeCvtFloatToInt(_DInst& insn, u32 source_bits) {
    auto& op0 = insn.ops[0];
    const u32 width = op0.size ? op0.size : 32;
    auto source = LoadSrcLo(insn, insn.ops[1]);
    auto result = __ VecFCvtFloatToInt(
            source, ir::Imm(source_bits), ir::Imm(width), ir::Imm(1u));
    Dst(insn, op0, result.SetType(width == 64 ? ir::ValueType::U64 : ir::ValueType::U32));
}

void X64Decoder::DecodePackedConvert(_DInst& insn, u32 kind) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecFCvtPacked(LoadSrcVec(insn, insn.ops[1]), ir::Imm(kind));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeCvtsd2ss(_DInst& insn) {
    // cvtsd2ss xmm, xmm/m64 (legacy SSE2): dst[31:0] = float(src[63:0]);
    // dst[63:32] UNCHANGED; dst[127:64] unchanged.
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto src = LoadSrcLo(insn, insn.ops[1]);
    auto old_lo = XmmLo(dst);
    auto new_f32 = __ VecFCvtScalar(src, ir::Imm(64));
    auto result = __ Or(__ And(old_lo, ir::Operand{ir::Imm(u64(0xFFFFFFFF00000000))}),
                        ir::Operand{__ And(new_f32, ir::Operand{ir::Imm(u64(0xFFFFFFFF))})});
    XmmLo(dst, result);
}

void X64Decoder::DecodeCvtss2sd(_DInst& insn) {
    // cvtss2sd xmm, xmm/m32: dst[63:0] = (double)(float)src[31:0]; high qword unchanged.
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto src = LoadSrcLo(insn, insn.ops[1]);
    XmmLo(dst, __ VecFCvtScalar(src, ir::Imm(32)));
}

void X64Decoder::DecodeScalarFloatOp(_DInst& insn, VecFloatOp op, u32 lane_bits) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto right = ScalarVOperandsEnabled()
                         ? LoadSrcScalarVec(insn, insn.ops[1], lane_bits)
                         : LoadSrcLo(insn, insn.ops[1]);
    ir::Value result;
    if (lane_bits == 64) {
        switch (op) {
            case VecFloatOp::Add: result = __ VecFAddScalar64(left, right); break;
            case VecFloatOp::Sub: result = __ VecFSubScalar64(left, right); break;
            case VecFloatOp::Mul: result = __ VecFMulScalar64(left, right); break;
            case VecFloatOp::Div: result = __ VecFDivScalar64(left, right); break;
        }
    } else {
        switch (op) {
            case VecFloatOp::Add: result = __ VecFAddScalar32(left, right); break;
            case VecFloatOp::Sub: result = __ VecFSubScalar32(left, right); break;
            case VecFloatOp::Mul: result = __ VecFMulScalar32(left, right); break;
            case VecFloatOp::Div: result = __ VecFDivScalar32(left, right); break;
        }
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

// `predicate` is the x86 SSE imm8 the mnemonic implies (0..7: eq, lt, le,
// unord, neq, nlt, nle, ord).  VecFCmpMask does not take an x86 encoding --
// it takes a relation set -- so it is translated through the SAME table the
// VEX path uses (fp_cmp_predicate.h), whose first eight rows ARE the SSE
// predicates.  The two paths therefore cannot drift.
void X64Decoder::DecodeFloatCompareMask(
        _DInst& insn, u32 lane_bits, u32 predicate, bool scalar) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto& op1 = insn.ops[1];
    ir::Value right;
    if (op1.type == O_REG) {
        right = XmmRead(static_cast<_RegisterType>(op1.index));
    } else if (!scalar) {
        right = MemLoad(ir::Operand{FlatAddress(insn, op1)},
                        ir::ValueType::V128,
                        TsoOrdered(insn));
    } else {
        auto raw = MemLoad(ir::Operand{FlatAddress(insn, op1)},
                           lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64,
                           TsoOrdered(insn));
        right = __ VecDup64(__ ZeroExtend64(raw)).SetType(ir::ValueType::V128);
    }
    auto result = __ VecFCmpMask(left,
                                 right,
                                 ir::Imm(lane_bits),
                                 ir::Imm(X86CmpPredicateToRelation(predicate)),
                                 ir::Imm(u32(scalar)));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeFloatMinMax(
        _DInst& insn, u32 lane_bits, bool maximum, bool scalar) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto left = XmmRead(dst);
    auto& op1 = insn.ops[1];
    ir::Value right;
    if (op1.type == O_REG) {
        right = XmmRead(static_cast<_RegisterType>(op1.index));
    } else if (!scalar) {
        right = MemLoad(ir::Operand{FlatAddress(insn, op1)},
                        ir::ValueType::V128,
                        TsoOrdered(insn));
    } else {
        auto raw = MemLoad(ir::Operand{FlatAddress(insn, op1)},
                           lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64,
                           TsoOrdered(insn));
        right = __ VecDup64(__ ZeroExtend64(raw)).SetType(ir::ValueType::V128);
    }
    auto result = __ VecFMinMax(left,
                                right,
                                ir::Imm(lane_bits),
                                ir::Imm(u32(maximum)),
                                ir::Imm(u32(scalar)));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeFloatUnary(
        _DInst& insn, u32 lane_bits, u32 kind, bool scalar) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto merge = XmmRead(dst);
    auto& op1 = insn.ops[1];
    ir::Value source;
    if (op1.type == O_REG) {
        source = XmmRead(static_cast<_RegisterType>(op1.index));
    } else if (!scalar) {
        source = MemLoad(ir::Operand{FlatAddress(insn, op1)},
                         ir::ValueType::V128,
                         TsoOrdered(insn));
    } else {
        auto raw = MemLoad(ir::Operand{FlatAddress(insn, op1)},
                           lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64,
                           TsoOrdered(insn));
        source = __ VecDup64(__ ZeroExtend64(raw)).SetType(ir::ValueType::V128);
    }
    auto result = __ VecFUnary(source,
                               merge,
                               ir::Imm(lane_bits),
                               ir::Imm(kind),
                               ir::Imm(u32(scalar)));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodePextrw(_DInst& insn) {
    // pextrw reg, xmm, imm8: zero-extended word (imm8 & 7) of the source xmm.
    auto src = static_cast<_RegisterType>(insn.ops[1].index);
    auto word =
            __ VecExtract16(XmmRead(src), ir::Imm(u32(insn.imm.byte))).SetType(ir::ValueType::U32);
    Dst(insn, insn.ops[0], word);
}

void X64Decoder::DecodePinsrw(_DInst& insn) {
    // pinsrw xmm, r/m16, imm8: insert the source word into lane (imm8 & 7).
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto& op1 = insn.ops[1];
    ir::Value word;
    if (op1.type == O_REG) {
        word = __ ZeroExtend64(R(static_cast<_RegisterType>(op1.index)));
    } else {
        word = __ ZeroExtend64(
                MemLoad(ir::Operand{FlatAddress(insn, op1)}, ir::ValueType::U16, false));
    }
    auto result = __ VecInsert16(XmmRead(dst), word, ir::Imm(u32(insn.imm.byte)))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodeMovddup(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecDup64(LoadSrcLo(insn, insn.ops[1])).SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodePalignr(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    u64 imm = insn.imm.byte;
    auto a_lo = XmmLo(dst);
    auto a_hi = XmmHi(dst);
    auto b = LoadSrcHalves(insn, insn.ops[1]);
    // 256-bit concat c = {b.lo, b.hi, a.lo, a.hi} (src low, dst high);
    // result = c >> (imm * 8), low 128 bits.
    if (imm >= 32) {
        XmmLo(dst, __ LoadImm(ir::Imm(u64(0))));
        XmmHi(dst, __ LoadImm(ir::Imm(u64(0))));
        return;
    }
    auto c = [&](u32 i) -> ir::Value {
        switch (i) {
            case 0:
                return b.lo;
            case 1:
                return b.hi;
            case 2:
                return a_lo;
            default:
                return a_hi;
        }
    };
    u32 q = u32(imm / 8);
    u32 s = u32((imm % 8) * 8);
    auto extract = [&](u32 i) -> ir::Value {
        // out qword i = bytes [imm + 8i, imm + 8i + 8) of the concat.
        u32 idx = q + i;
        if (idx > 3) {
            return __ LoadImm(ir::Imm(u64(0)));
        }
        if (s == 0) {
            return c(idx);
        }
        auto lo_part = __ LsrImm(c(idx), ir::Imm(u64(s)));
        if (idx + 1 > 3) {
            return lo_part;
        }
        return __ Or(lo_part, ir::Operand{__ LslImm(c(idx + 1), ir::Imm(u64(64 - s)))});
    };
    XmmLo(dst, extract(0));
    XmmHi(dst, extract(1));
}

void X64Decoder::DecodePshufb(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecTableLookup8(XmmRead(dst), LoadSrcVec(insn, insn.ops[1]))
                          .SetType(ir::ValueType::V128);
    XmmWrite(dst, result);
}

void X64Decoder::DecodePmovmskb(_DInst& insn) {
    auto src = static_cast<_RegisterType>(insn.ops[1].index);
    auto mask = __ VecMovMask(XmmRead(src), ir::Imm(8u)).SetType(ir::ValueType::U32);
    Dst(insn, insn.ops[0], mask);
}

void X64Decoder::DecodeMxcsr(_DInst& insn, bool load) {
    auto addr = FlatAddress(insn, insn.ops[0]);
    ir::Uniform uni_mxcsr{offsetof(ThreadContext64, mxcsr), ir::ValueType::U32};
    if (load) {
        auto v = __ LoadMemory(ir::Operand{addr}).SetType(ir::ValueType::U32);
        __ StoreUniform(uni_mxcsr, v);
        if (sse_afp_nan_) {
            __ SetLocation(ir::Lambda{ir::Imm{pc}});
            __ ReturnToDispatcher();
        }
    } else {
        __ StoreMemory(ir::Operand{addr}, __ LoadUniform(uni_mxcsr));
    }
}

void X64Decoder::DecodeFxsave(_DInst& insn, bool restore) {
    auto addr = FlatAddress(insn, insn.ops[0]);
    auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
    ir::Uniform uni_mxcsr{offsetof(ThreadContext64, mxcsr), ir::ValueType::U32};
    constexpr s32 kXsaveXmmOff = 160;  // xmm0 starts at byte 160 in the fxsave area
    if (!restore) {
        // Zero + save the architectural x87 area first, then overwrite the
        // live MXCSR and XMM fields.
        auto status = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&X87Fxsave)}},
                                    context,
                                    addr)
                              .SetType(ir::ValueType::U64);
        // An unmapped save area is a guest #PF, not a silent no-op. The
        // StoreMemory calls below would fault on their own, but only after the
        // x87 half had already been skipped -- and FXSAVE with RFBM covering
        // only x87 would not fault at all.
        RaiseIfGuestFault(status, insn_pc);
        __ StoreMemory(ir::Operand{addr, 24, ir::OperandPlus}, __ LoadUniform(uni_mxcsr));
        for (u32 i = 0; i < 16; ++i) {
            ir::Uniform uni_xmm{u32(offsetof(ThreadContext64, xmms) + i * sizeof(Xmm)),
                                ir::ValueType::V128};
            __ StoreMemory(ir::Operand{addr, kXsaveXmmOff + s32(16 * i), ir::OperandPlus},
                           __ LoadUniform(uni_xmm));
        }
    } else {
        auto status = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&X87Fxrstor)}},
                                    context,
                                    addr)
                              .SetType(ir::ValueType::U64);
        RaiseIfGuestFault(status, insn_pc);
        auto mx = __ LoadMemory(ir::Operand{addr, 24, ir::OperandPlus}).SetType(ir::ValueType::U32);
        __ StoreUniform(uni_mxcsr, mx);
        for (u32 i = 0; i < 16; ++i) {
            ir::Uniform uni_xmm{u32(offsetof(ThreadContext64, xmms) + i * sizeof(Xmm)),
                                ir::ValueType::V128};
            auto v = __ LoadMemory(ir::Operand{addr, kXsaveXmmOff + s32(16 * i), ir::OperandPlus})
                             .SetType(ir::ValueType::V128);
            __ StoreUniform(uni_xmm, v);
        }
        if (sse_afp_nan_) {
            __ SetLocation(ir::Lambda{ir::Imm{pc}});
            __ ReturnToDispatcher();
        }
    }
}

void X64Decoder::DecodeUcomis(_DInst& insn, u32 lane_bits) {
    const bool compact = FlagsFcmpFuseEnabled() && FlagsFcmpCompactEnabled();
    auto a = compact ? XmmScalarV(static_cast<_RegisterType>(insn.ops[0].index), lane_bits)
                     : XmmLo(static_cast<_RegisterType>(insn.ops[0].index));
    auto b = compact ? LoadSrcScalarVec(insn, insn.ops[1], lane_bits)
                     : LoadSrcLo(insn, insn.ops[1]);
    auto f = __ VecFCmp(a, b, ir::Imm(lane_bits), ir::Imm(u32(compact)))
                     .SetType(ir::ValueType::U64);
    if (FlagsFcmpFuseEnabled()) {
        PublishFCmpFlags(f);
        return;
    }
    // OF / SF / AF cleared; ZF / PF / CF from the compare result.
    __ ClearFlags(ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::AuxiliaryCarry);
    auto one = __ LoadImm(ir::Imm(u64(1)));
    auto zero = __ LoadImm(ir::Imm(u64(0)));
    // PF: parity flag of low byte; 0 has even parity (PF=1), 1 has PF=0.
    auto pf = __ And(__ LsrImm(f, ir::Imm(1u)), ir::Operand{ir::Imm(u64(1))});
    auto pv = __ Select(__ TestNotZero(pf), zero, one);
    __ SaveFlags(__ Or(pv, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Parity);
    // ZF: host value 0 sets Z, 1 clears it.  Keep this NZ write after PF so
    // the logical flag producer does not overwrite the already-saved parity.
    auto zf = __ And(__ LsrImm(f, ir::Imm(2u)), ir::Operand{ir::Imm(u64(1))});
    auto zv = __ Select(__ TestNotZero(zf), zero, one);
    __ SaveFlags(__ Or(zv, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Zero);
    // Encode CF as the carry-out of (~0 + cf), letting the normal flag
    // producer path materialize host C and preserving the frontend's carry
    // polarity state for following ADC/SBB instructions.
    auto cf = __ And(f, ir::Operand{ir::Imm(u64(1))});
    auto cv = __ Add(__ LoadImm(ir::Imm(~u64(0))), ir::Operand{cf});
    __ SaveFlags(cv, ir::Flags::Carry);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}

void X64Decoder::DecodeUcomisd(_DInst& insn) { DecodeUcomis(insn, 64); }

}  // namespace swift::x86
