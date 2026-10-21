#include "translator.h"

#include "runtime/backend/arm64/defines.h"
#include "runtime/common/sse42str_result.h"
#include "runtime/frontend/x86/sse42str_helper.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

// Inline PCMPISTRI/M common core.  imm8 is a translation-time constant, so
// each aggregation gets a dedicated NEON shape; equal-any uses a compact
// runtime loop over the first operand while the other three are straight-line.
// The packed result layout is shared with decoder_sse42str.cc:
//   [15:0] IntRes2, [23:16] index, [24] SF, [25] ZF, [26] CF, [27] OF.
void JitTranslator::EmitSse42Str(ir::Inst* inst) {
    const auto a = context.V(inst->GetArg<ir::Value>(0));
    const auto b = context.V(inst->GetArg<ir::Value>(1));
    const u32 imm = u32(inst->GetArg<ir::Imm>(2).Get()) & 0xFFu;
    const bool words = (imm & 0x01u) != 0;
    const bool is_signed = (imm & 0x02u) != 0;
    const u32 aggregation = (imm >> 2) & 3u;
    const u32 polarity = (imm >> 4) & 3u;
    const bool most_significant = (imm & 0x40u) != 0;
    const u32 n = words ? 8u : 16u;
    const u32 all = (1u << n) - 1u;

    auto result = context.W(ir::Value{inst});
    if (const auto target =
                swift::x86::Sse42StrVectorHelperAddress(u8(imm));
        target) {
        EmitSse42StrVectorCall(
                inst, a, b, result, target, u8(imm));
        return;
    }
    const auto publish_flags = cur_block
            ? gpr_plan->consumers->string_flags.at(inst)
            : ir::Flags::All;
    if (imm == 0x3au && a.GetCode() == b.GetCode()) {
        const auto length = context.GetTmpX();
        const auto scalar = context.GetTmpX();
        const auto zero = context.GetTmpV();
        __ Cmeq(zero.V16B(), a.V16B(), 0);
        __ Shrn(zero.V8B(), zero.V8H(), 4);
        __ Fmov(length, zero.D());
        __ Rbit(length, length);
        __ Clz(length, length);
        __ Lsr(length, length, 2);

        __ Mov(result, 1);
        __ Lsl(result, result, length.W());
        __ Sub(result, result, 1);
        __ Eor(result, result, all);
        __ Orr(result, result, Operand{length.W(), LSL, 16});

        const auto length_flags = publish_flags &
                (ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Carry);
        if (True(length_flags)) {
            __ Cmp(length.W(), n);
            __ Cset(scalar.W(), lt);
            if (True(length_flags & ir::Flags::Negate)) {
                __ Orr(result, result,
                       Operand{scalar.W(), LSL, sse42str::kSignBit});
            }
            if (True(length_flags & ir::Flags::Zero)) {
                __ Orr(result, result,
                       Operand{scalar.W(), LSL, sse42str::kZeroBit});
            }
            if (True(length_flags & ir::Flags::Carry)) {
                __ Orr(result, result,
                       Operand{scalar.W(), LSL, sse42str::kCarryBit});
            }
        }
        if (True(publish_flags & ir::Flags::Overflow)) {
            __ Cmp(length.W(), 0);
            __ Cset(scalar.W(), eq);
            __ Orr(result, result,
                   Operand{scalar.W(), LSL, sse42str::kOverflowBit});
        }
        return;
    }
    auto len1 = context.GetTmpX();
    auto len2 = context.GetTmpX();
    auto scalar = context.GetTmpX();
    auto scalar2 = context.GetTmpX();

    auto acc = context.GetTmpV();
    auto row = context.GetTmpV();
    auto aux = context.GetTmpV();
    auto ones = context.GetTmpV();

    // VIXL's two-lane Movi macro acquires an extra implicit GPR.  Reuse the
    // declared scalar temporary after its current value is dead instead, so
    // the emitter's real peak remains the four GPRs advertised to the RA.
    const auto emit_u128 = [&](const VRegister& dst, u64 hi, u64 lo) {
        __ Mov(scalar, lo);
        __ Ins(dst.V2D(), 0, scalar);
        __ Mov(scalar, hi);
        __ Ins(dst.V2D(), 1, scalar);
    };

    // First zero element, or n when there is no zero.  The byte form packs
    // two compare lanes into the low/high nibbles of each narrowed byte; the
    // word form narrows one compare lane per byte.  RBIT+CLZ naturally gives
    // 64 for an all-zero packed value, which becomes exactly 16 or 8.
    const auto emit_length = [&](const VRegister& src, const XRegister& dst) {
        if (words) {
            __ Cmeq(row.V8H(), src.V8H(), 0);
            __ Xtn(row.V8B(), row.V8H());
            __ Fmov(dst, row.D());
            __ Rbit(dst, dst);
            __ Clz(dst, dst);
            __ Lsr(dst, dst, 3);
        } else {
            __ Cmeq(row.V16B(), src.V16B(), 0);
            __ Shrn(row.V8B(), row.V8H(), 4);
            __ Fmov(dst, row.D());
            __ Rbit(dst, dst);
            __ Clz(dst, dst);
            __ Lsr(dst, dst, 2);
        }
    };
    emit_length(a, len1);
    emit_length(b, len2);

    Label aggregate_done;
    switch (aggregation) {
        case 0: {  // equal any
            // Walk the first operand at runtime instead of materializing one
            // compare row per possible element.  Besides being much smaller,
            // this is the important strcspn shape: its reject set is normally
            // only a few elements long.
            Label equal_any_loop;
            __ Eor(acc.V16B(), acc.V16B(), acc.V16B());
            __ Orr(row.V16B(), a.V16B(), a.V16B());
            __ Mov(scalar.W(), len1.W());
            __ Cbz(scalar.W(), &aggregate_done);
            __ Bind(&equal_any_loop);
            if (words) {
                __ Dup(aux.V8H(), row.V8H(), 0);
                __ Cmeq(aux.V8H(), aux.V8H(), b.V8H());
                __ Ext(row.V16B(), row.V16B(), row.V16B(), 2);
            } else {
                __ Dup(aux.V16B(), row.V16B(), 0);
                __ Cmeq(aux.V16B(), aux.V16B(), b.V16B());
                __ Ext(row.V16B(), row.V16B(), row.V16B(), 1);
            }
            __ Orr(acc.V16B(), acc.V16B(), aux.V16B());
            __ Subs(scalar.W(), scalar.W(), 1);
            __ B(&equal_any_loop, ne);
            break;
        }
        case 1:  // ranges
            __ Eor(acc.V16B(), acc.V16B(), acc.V16B());
            for (u32 i = 0; i + 1 < n; i += 2) {
                __ Cmp(len1.W(), i + 1);
                __ B(&aggregate_done, ls);
                if (words) {
                    __ Dup(row.V8H(), a.V8H(), i);
                    if (is_signed) {
                        __ Cmge(aux.V8H(), b.V8H(), row.V8H());
                    } else {
                        __ Cmhs(aux.V8H(), b.V8H(), row.V8H());
                    }
                    __ Dup(row.V8H(), a.V8H(), i + 1);
                    if (is_signed) {
                        __ Cmge(row.V8H(), row.V8H(), b.V8H());
                    } else {
                        __ Cmhs(row.V8H(), row.V8H(), b.V8H());
                    }
                } else {
                    __ Dup(row.V16B(), a.V16B(), i);
                    if (is_signed) {
                        __ Cmge(aux.V16B(), b.V16B(), row.V16B());
                    } else {
                        __ Cmhs(aux.V16B(), b.V16B(), row.V16B());
                    }
                    __ Dup(row.V16B(), a.V16B(), i + 1);
                    if (is_signed) {
                        __ Cmge(row.V16B(), row.V16B(), b.V16B());
                    } else {
                        __ Cmhs(row.V16B(), row.V16B(), b.V16B());
                    }
                }
                __ And(row.V16B(), row.V16B(), aux.V16B());
                __ Orr(acc.V16B(), acc.V16B(), row.V16B());
            }
            break;
        case 2:  // equal each
            if (words) {
                __ Cmeq(acc.V8H(), a.V8H(), b.V8H());
            } else {
                __ Cmeq(acc.V16B(), a.V16B(), b.V16B());
            }
            break;
        default: {  // equal ordered
            // valid2[j] = j < len2.  The arbitrary constants are lane
            // indexes in architectural little-endian element order.
            if (words) {
                emit_u128(aux, 0x0007000600050004ull, 0x0003000200010000ull);
                __ Dup(row.V8H(), len2.W());
                __ Cmhi(aux.V8H(), row.V8H(), aux.V8H());
            } else {
                emit_u128(aux, 0x0f0e0d0c0b0a0908ull, 0x0706050403020100ull);
                __ Dup(row.V16B(), len2.W());
                __ Cmhi(aux.V16B(), row.V16B(), aux.V16B());
            }
            __ Movi(ones.V16B(), 0xFF);
            __ Orr(acc.V16B(), ones.V16B(), ones.V16B());
            for (u32 i = 0; i < n; ++i) {
                __ Cmp(len1.W(), i);
                __ B(&aggregate_done, ls);
                if (words) {
                    __ Dup(row.V8H(), a.V8H(), i);
                    __ Cmeq(row.V8H(), row.V8H(), b.V8H());
                } else {
                    __ Dup(row.V16B(), a.V16B(), i);
                    __ Cmeq(row.V16B(), row.V16B(), b.V16B());
                }
                __ And(row.V16B(), row.V16B(), aux.V16B());
                __ Ext(row.V16B(), row.V16B(), ones.V16B(), i * (words ? 2u : 1u));
                __ And(acc.V16B(), acc.V16B(), row.V16B());
            }
            break;
        }
    }
    __ Bind(&aggregate_done);

    // Collapse all-ones/all-zero comparison lanes to the architectural n-bit
    // IntRes1.  ARM64 has no PMOVMSKB, so use bit weights and horizontal add.
    if (words) {
        emit_u128(row, 0x0080004000200010ull, 0x0008000400020001ull);
        __ And(acc.V16B(), acc.V16B(), row.V16B());
        __ Addv(acc.H(), acc.V8H());
        __ Umov(result, acc.V8H(), 0);
    } else {
        EmitByteMovMask(acc, result, acc, row);
    }

    const auto emit_valid_bits = [&](const XRegister& length, const Register& out) {
        __ Mov(out, 1);
        __ Lsl(out, out, length.W());
        __ Sub(out, out, 1);
    };

    // Apply the SDM validity override after aggregation where it is scalar
    // algebra.  Equal ordered already applied valid2 to every row above.
    if (aggregation == 0 || aggregation == 1) {
        emit_valid_bits(len2, scalar.W());
        __ And(result, result, scalar.W());
    } else if (aggregation == 2) {
        emit_valid_bits(len1, scalar.W());
        emit_valid_bits(len2, scalar2.W());
        __ And(result, result, scalar.W());
        __ And(result, result, scalar2.W());
        __ Orr(scalar.W(), scalar.W(), scalar2.W());
        __ Mvn(scalar.W(), scalar.W());
        __ And(scalar.W(), scalar.W(), all);
        __ Orr(result, result, scalar.W());
    }

    // Polarity.  Encodings 00 and 10 are both positive; masked-negative
    // inverts only valid elements of the second operand.
    if (polarity == 1) {
        __ Eor(result, result, all);
    } else if (polarity == 3) {
        emit_valid_bits(len2, scalar.W());
        __ Eor(result, result, scalar.W());
    }

    // Index: least/most significant set bit, or n for an empty IntRes2.
    const bool publish_carry = True(publish_flags & ir::Flags::Carry);
    if (!most_significant && publish_carry) {
        __ Cmp(result, 0);
    }
    if (most_significant) {
        __ Clz(scalar.W(), result);
        __ Eor(scalar.W(), scalar.W(), 31);
    } else {
        // A sentinel at bit n makes the empty result naturally select n.
        __ Rbit(scalar.W(), result);
        __ Orr(scalar.W(), scalar.W(), u32(1) << (31u - n));
        __ Clz(scalar.W(), scalar.W());
    }
    if (most_significant) {
        __ Cmp(result, 0);
        __ Mov(scalar2.W(), n);
        __ Csel(scalar.W(), scalar.W(), scalar2.W(), ne);
    }
    __ Orr(result, result, Operand{scalar.W(), LSL, 16});

    if (publish_carry) {
        __ Cset(scalar.W(), ne);
        __ Orr(result, result,
               Operand{scalar.W(), LSL, sse42str::kCarryBit});
    }

    if (True(publish_flags & ir::Flags::Negate)) {
        __ Cmp(len1.W(), n);
        __ Cset(scalar.W(), lt);
        __ Orr(result, result,
               Operand{scalar.W(), LSL, sse42str::kSignBit});
    }
    if (True(publish_flags & ir::Flags::Zero)) {
        __ Cmp(len2.W(), n);
        __ Cset(scalar.W(), lt);
        __ Orr(result, result,
               Operand{scalar.W(), LSL, sse42str::kZeroBit});
    }
    if (True(publish_flags & ir::Flags::Overflow)) {
        __ And(scalar.W(), result, 1);
        __ Orr(result, result,
               Operand{scalar.W(), LSL, sse42str::kOverflowBit});
    }
}

void JitTranslator::EmitPublishSse42StrFlags(ir::Inst* inst) {
    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
    FlushFlags();
    InvalidateFlagsToken();

    const auto requested = inst->GetArg<ir::Flags>(1);
    const auto packed = context.W(inst->GetArg<ir::Value>(0));
    const auto scratch = context.GetSharedTmpX();
    if ((requested & ir::Flags::NZCV) == ir::Flags::NZCV) {
        __ Bfc(flags, HostFlagsBit::V, 4);
        __ Ubfx(scratch.W(), packed, sse42str::kSignBit, 4);
        __ Rbit(scratch.W(), scratch.W());
        __ Orr(flags, flags, scratch);
    } else {
        const auto publish = [&](ir::Flags flag, u32 source, u32 target) {
            if (True(requested & flag)) {
                __ Ubfx(scratch.W(), packed, source, 1);
                __ Bfi(flags, scratch, target, 1);
            }
        };
        publish(ir::Flags::Negate, sse42str::kSignBit, HostFlagsBit::N);
        publish(ir::Flags::Zero, sse42str::kZeroBit, HostFlagsBit::Z);
        publish(ir::Flags::Carry, sse42str::kCarryBit, HostFlagsBit::C);
        publish(ir::Flags::Overflow, sse42str::kOverflowBit, HostFlagsBit::V);
    }
    if (True(requested & ir::Flags::AuxiliaryCarry)) {
        __ Bfc(flags, HostFlagsBit::AuxiliaryCarry, 1);
    }
    if (True(requested & ir::Flags::Parity)) {
        __ Bfc(flags, HostFlagsBit::ParityByte, 8);
        __ Orr(flags, flags, 1);
    }
}

}  // namespace swift::runtime::backend::arm64
