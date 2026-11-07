#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64::detail {
void Writer::HandleVecAvgFamily(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto left = VectorValue(c.Take(), 0), right = VectorValue(c.Take(), 1);
    const auto bits = IntegerImmediate(c.Take());
    LaneSize(bits, false);
    __ urhadd(Lanes(ctx.destination, bits), Lanes(left, bits), Lanes(right, bits));
}

void Writer::HandleVecSatAddFamily(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto left = VectorValue(c.Take(), 0), right = VectorValue(c.Take(), 1);
    const auto bits = IntegerImmediate(c.Take()), sign = IntegerImmediate(c.Take());
    LaneSize(bits);
    if (sign > 1) Reject(CompileError::UnsupportedOperand);
    const auto out = Lanes(ctx.destination, bits), a = Lanes(left, bits), b = Lanes(right, bits);
    if (ctx.operation.opcode == OpCode::VecSatSub) {
        if (sign)
            __ sqsub(out, a, b);
        else
            __ uqsub(out, a, b);
    } else {
        if (sign)
            __ sqadd(out, a, b);
        else
            __ uqadd(out, a, b);
    }
}

void Writer::HandleVecMulHigh16Family(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto left = VectorValue(c.Take(), 0), right = VectorValue(c.Take(), 1);
    const auto sign = IntegerImmediate(c.Take());
    if (sign > 1) Reject(CompileError::UnsupportedOperand);
    // v0 can be reused after both low-half products have been read. Keep
    // high products out of the spill-result register until SHRN2 consumes it.
    const auto lo = VectorTemporary(), hi = 0u;
    if (sign) {
        __ smull(Lanes(lo, 32), Lanes(left, 16, 64), Lanes(right, 16, 64));
        __ smull2(Lanes(hi, 32), Lanes(left, 16), Lanes(right, 16));
    } else {
        __ umull(Lanes(lo, 32), Lanes(left, 16, 64), Lanes(right, 16, 64));
        __ umull2(Lanes(hi, 32), Lanes(left, 16), Lanes(right, 16));
    }
    __ shrn(Lanes(ctx.destination, 16, 64), Lanes(lo, 32), 16);
    __ shrn2(Lanes(ctx.destination, 16), Lanes(hi, 32), 16);
}

void Writer::HandleVecAbsDiffSum8Family(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto left = VectorValue(c.Take(), 0), right = VectorValue(c.Take(), 1),
               temp = VectorTemporary();
    __ uabd(Lanes(temp, 8), Lanes(left, 8), Lanes(right, 8));
    __ uaddlp(Lanes(temp, 16), Lanes(temp, 8));
    __ uaddlp(Lanes(temp, 32), Lanes(temp, 16));
    __ uaddlp(Lanes(ctx.destination, 64), Lanes(temp, 32));
}

void Writer::HandleVecShiftLeftFamily(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto input = VectorValue(c.Take(), 0);
    const auto amount = c.Take();
    const auto bits = IntegerImmediate(c.Take());
    LaneSize(bits);
    const bool arithmetic = ctx.operation.opcode == OpCode::VecShiftRightArithmetic;
    const bool left = ctx.operation.opcode == OpCode::VecShiftLeft;
    if (View(amount) == Type::V128) {
        const auto counts = VectorValue(amount, 1), maximum = VectorTemporary();
        Immediate(17, arithmetic ? bits - 1 : bits);
        __ dup(Lanes(maximum, bits), R(17, bits == 64 ? 64 : 32));
        unsigned limited = maximum;
        if (bits == 64) {
            limited = VectorTemporary(true);
            __ cmhi(Lanes(limited, 64), Lanes(counts, 64), Lanes(maximum, 64));
            __ bsl(Lanes(limited, 8), Lanes(maximum, 8), Lanes(counts, 8));
        } else
            __ umin(Lanes(limited, bits), Lanes(counts, bits), Lanes(maximum, bits));
        if (!left) __ neg(Lanes(limited, bits), Lanes(limited, bits));
        if (arithmetic)
            __ sshl(Lanes(ctx.destination, bits), Lanes(input, bits), Lanes(limited, bits));
        else
            __ ushl(Lanes(ctx.destination, bits), Lanes(input, bits), Lanes(limited, bits));
        return;
    }
    Move(17, Read(amount, 17));
    // The complete low qword count, not its low byte, controls saturation.
    __ lsr(v::x16, v::x17, std::countr_zero(static_cast<unsigned>(bits)));
    const auto in_range = masm.Here();
    __ cbz(v::x16, std::int64_t{0});
    u32 done = UINT32_MAX;
    if (arithmetic)
        Immediate(17, bits - 1);
    else {
        __ eor(Lanes(ctx.destination, 8), Lanes(ctx.destination, 8), Lanes(ctx.destination, 8));
        done = masm.Here();
        __ b(std::int64_t{0});
    }
    masm.Patch(in_range, masm.Here());
    if (!left) __ neg(v::x17, Operand(v::x17));
    __ dup(Lanes(1, bits), R(17, bits == 64 ? 64 : 32));
    if (arithmetic)
        __ sshl(Lanes(ctx.destination, bits), Lanes(input, bits), Lanes(1, bits));
    else
        __ ushl(Lanes(ctx.destination, bits), Lanes(input, bits), Lanes(1, bits));
    if (done != UINT32_MAX) masm.Patch(done, masm.Here());
}

void Writer::HandleVecDupPairs32Family(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto input = VectorValue(c.Take(), 0);
    const auto high = IntegerImmediate(c.Take());
    if (high > 1) Reject(CompileError::UnsupportedOperand);
    if (high)
        __ trn2(Lanes(ctx.destination, 32), Lanes(input, 32), Lanes(input, 32));
    else
        __ trn1(Lanes(ctx.destination, 32), Lanes(input, 32), Lanes(input, 32));
}

void Writer::HandleVecInsert16Family(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto input = VectorValue(c.Take(), 0), value = Read(c.Take(), 16, 16);
    const auto lane = IntegerImmediate(c.Take());
    if (lane >= 8) Reject(CompileError::UnsupportedOperand);
    VectorMove(ctx.destination, input);
    __ ins(Lanes(ctx.destination, 16), lane, W(value));
}

void Writer::HandleVecTableLookup8Family(OpContext& ctx) {
    auto& c = ctx.cursor;
    const auto input = VectorValue(c.Take(), 0), indices = VectorValue(c.Take(), 1);
    const auto mask = VectorTemporary(), selected = VectorTemporary(true);
    Immediate(17, UINT64_C(0x8f8f8f8f8f8f8f8f));
    __ dup(Lanes(mask, 64), v::x17);
    __ and_(Lanes(selected, 8), Lanes(indices, 8), Lanes(mask, 8));
    __ tbl(Lanes(ctx.destination, 8), Lanes(input, 8), Lanes(selected, 8));
}
}  // namespace swift::runtime::backend::arm64::detail

#undef __
