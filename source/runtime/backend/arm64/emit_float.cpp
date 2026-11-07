#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {

void Writer::LoadMxcsrFlushControl() {
    if (options.mxcsr_flush_mode != UINT8_MAX) return;
    const auto offset = options.mxcsr_flush_offset;
    if (!(offset & 3) && offset / 4 <= 4095)
        Memory(true, 32, 0, 19, offset / 4);
    else {
        Immediate(17, offset);
        __ add(v::x17, v::x19, Operand(v::x17));
        Memory(true, 32, 0, 17);
    }
}

void Writer::FlushScalarDenormal(unsigned value,
                                 unsigned control_bit,
                                 unsigned lane,
                                 unsigned lane_bits,
                                 unsigned lane_count) {
    // Known software controls belong to this whole native unit. Otherwise
    // x0 holds the actual raw MXCSR controls. Neither branch changes NZCV.
    u32 disabled = UINT32_MAX;
    if (options.mxcsr_flush_mode != UINT8_MAX) {
        if (!(options.mxcsr_flush_mode & (control_bit == 6 ? 1u : 2u))) return;
    } else {
        disabled = static_cast<u32>(output.words.size());
        __ tbz(v::w0, control_bit, std::int64_t{0});  // TBZ w0,control_bit,done.
    }
    for (unsigned i = lane; i < lane + lane_count; ++i) {
        const bool single = lane_bits == 32;
        __ umov(single ? W(16) : X(16), Lanes(value, lane_bits), i);  // UMOV W/X16,lane.
        Extract(17, 16, single ? 23 : 52, single ? 8 : 11);
        const auto normal = static_cast<u32>(output.words.size());
        __ cbnz(v::x17, std::int64_t{0});  // CBNZ x17,next: zero exponent includes existing zeros.
        __ and_(v::x16,
                v::x16,
                Operand(u64{1} << (lane_bits - 1)));                 // Keep only the IEEE sign bit.
        __ ins(Lanes(value, lane_bits), i, single ? W(16) : X(16));  // INS preserves other lanes.
        masm.Patch(normal, masm.Here());
    }
    const auto done = static_cast<u32>(output.words.size());
    if (disabled != UINT32_MAX) masm.Patch(disabled, done);
}

void Writer::ConvertFloatToInteger(unsigned destination,
                                   unsigned raw,
                                   unsigned float_bits,
                                   unsigned integer_bits,
                                   bool truncate) {
    const auto fp = float_bits == 32 ? S(0) : D(0);
    __ fmov(fp, R(raw, float_bits));
    if (!truncate) {
        if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
            LoadMxcsrFlushControl();
            FlushScalarDenormal(0, 6, 0, float_bits, 1);
        }
        __ frinti(fp, fp);  // Active guest RC; no sticky/trap state is published.
    }
    __ fmov(R(16, float_bits), fp);
    Extract(17, 16, float_bits == 32 ? 23 : 52, float_bits == 32 ? 8 : 11);
    __ sub(v::x17, v::x17, Operand((float_bits == 32 ? 127u : 1023u) + integer_bits - 1));
    __ fcvtzs(R(destination, integer_bits == 64 ? 64 : 32), fp);
    const auto valid = masm.Here();
    __ tbnz(v::x17, 63, std::int64_t{0});
    Immediate(destination, u64{1} << (integer_bits - 1));
    masm.Patch(valid, masm.Here());
    if (integer_bits < 32) Narrow(destination, destination, integer_bits);
}

void Writer::TruncateDoubleToInteger(unsigned destination, unsigned raw, unsigned bits) {
    ConvertFloatToInteger(destination, raw, 64, bits, true);
}

void Writer::ConvertScalarFloat(unsigned destination, unsigned raw, unsigned source_bits) {
    const auto input = source_bits == 32 ? S(0) : D(0);
    const auto result_bits = source_bits == 32 ? 64u : 32u;
    const auto result = result_bits == 32 ? S(0) : D(0);
    __ fmov(input, R(raw, source_bits));
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        LoadMxcsrFlushControl();
        FlushScalarDenormal(0, 6, 0, source_bits, 1);
        __ fcvt(result, input);
        FlushScalarDenormal(0, 15, 0, result_bits, 1);
    } else
        __ fcvt(result, input);
    __ fmov(R(destination, result_bits), result);
}

void Writer::HandleVecFCvtIntToFloatFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto input = cursor.Take();
    const auto source_bits = IntegerImmediate(cursor.Take()),
               float_bits = IntegerImmediate(cursor.Take());
    if ((source_bits != 16 && source_bits != 32 && source_bits != 64) ||
        (float_bits != 32 && float_bits != 64) || Width(View(input)) != source_bits ||
        ctx.width != float_bits)
        Reject(CompileError::UnsupportedOperand);
    auto value = RawScalar(input, 16);
    if (source_bits == 16) {
        __ sxth(v::w16, W(value));
        value = 16;
    }
    const auto fp = float_bits == 32 ? S(0) : D(0);
    __ scvtf(fp, R(value, source_bits == 64 ? 64 : 32));
    __ fmov(R(ctx.destination, float_bits), fp);
}

void Writer::HandleVecFCvtFloatToIntFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto input = cursor.Take();
    const auto float_bits = IntegerImmediate(cursor.Take()),
               integer_bits = IntegerImmediate(cursor.Take());
    const auto truncate = IntegerImmediate(cursor.Take());
    if ((float_bits != 32 && float_bits != 64) ||
        (integer_bits != 16 && integer_bits != 32 && integer_bits != 64) || truncate > 1 ||
        Width(View(input)) != float_bits || ctx.width != integer_bits)
        Reject(CompileError::UnsupportedOperand);
    ConvertFloatToInteger(
            ctx.destination, RawScalar(input, 16), float_bits, integer_bits, truncate != 0);
}

void Writer::HandleVecFCvtScalarFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto input = cursor.Take();
    const auto bits = IntegerImmediate(cursor.Take());
    if ((bits != 32 && bits != 64) || Width(View(input)) != bits ||
        ctx.width != (bits == 32 ? 64u : 32u))
        Reject(CompileError::UnsupportedOperand);
    ConvertScalarFloat(ctx.destination, RawScalar(input, 16), bits);
}

void Writer::HandleVecFAddFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto vector = ctx.vector;
    const auto first_temp = VectorTemporary();
    const auto second_temp = VectorTemporary(true);
    if (!vector) Reject(CompileError::UnsupportedType);
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto bits = IntegerImmediate(cursor.Take());
    if (bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    const auto emit = [&](unsigned dst, unsigned lhs, unsigned rhs) {
        const auto vd = Lanes(dst, bits), vn = Lanes(lhs, bits), vm = Lanes(rhs, bits);
        if (operation.opcode == OpCode::VecFAdd)
            __ fadd(vd, vn, vm);
        else if (operation.opcode == OpCode::VecFSub)
            __ fsub(vd, vn, vm);
        else if (operation.opcode == OpCode::VecFMul)
            __ fmul(vd, vn, vm);
        else
            __ fdiv(vd, vn, vm);
    };
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        VectorMove(first_temp, left);
        VectorMove(second_temp, right);
        LoadMxcsrFlushControl();
        const auto lanes = static_cast<unsigned>(128 / bits);
        FlushScalarDenormal(first_temp, 6, 0, static_cast<unsigned>(bits), lanes);
        FlushScalarDenormal(second_temp, 6, 0, static_cast<unsigned>(bits), lanes);
        emit(first_temp, first_temp, second_temp);
        FlushScalarDenormal(first_temp, 15, 0, static_cast<unsigned>(bits), lanes);
        VectorMove(destination, first_temp);
    } else
        emit(destination, left, right);
}

void Writer::HandleVecFAddScalar64Family(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto opcode = ctx.operation.opcode;
    const bool single = opcode == OpCode::VecFAddScalar32 || opcode == OpCode::VecFSubScalar32 ||
                        opcode == OpCode::VecFMulScalar32 || opcode == OpCode::VecFDivScalar32;
    const unsigned bits = single ? 32 : 64;
    const auto first = VectorTemporary(), second = VectorTemporary(true);
    const auto emit = [&](unsigned a, unsigned b) {
        const auto out = single ? S(first) : D(first), lhs = single ? S(a) : D(a),
                   rhs = single ? S(b) : D(b);
        if (opcode == OpCode::VecFAddScalar32 || opcode == OpCode::VecFAddScalar64)
            __ fadd(out, lhs, rhs);
        else if (opcode == OpCode::VecFSubScalar32 || opcode == OpCode::VecFSubScalar64)
            __ fsub(out, lhs, rhs);
        else if (opcode == OpCode::VecFMulScalar32 || opcode == OpCode::VecFMulScalar64)
            __ fmul(out, lhs, rhs);
        else
            __ fdiv(out, lhs, rhs);
    };
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        VectorMove(first, left);
        VectorMove(second, right);
        LoadMxcsrFlushControl();
        FlushScalarDenormal(first, 6, 0, bits, 1);
        FlushScalarDenormal(second, 6, 0, bits, 1);
        emit(first, second);
        FlushScalarDenormal(first, 15, 0, bits, 1);
    } else
        emit(left, right);
    VectorMove(ctx.destination, left);
    __ ins(Lanes(ctx.destination, bits), 0, Lanes(first, bits), 0);
}

void Writer::HandleVecFCmpFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto first_temp = VectorTemporary();
    const auto second_temp = VectorTemporary(true);
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto bits = IntegerImmediate(cursor.Take());
    if (bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    unsigned a = left, b = right;
    if (options.mxcsr_flush_offset != UINT32_MAX &&
        (options.mxcsr_flush_mode == UINT8_MAX || (options.mxcsr_flush_mode & 1u))) {
        VectorMove(first_temp, left);
        VectorMove(second_temp, right);
        LoadMxcsrFlushControl();
        FlushScalarDenormal(first_temp, 6, 0, bits, 1);
        FlushScalarDenormal(second_temp, 6, 0, bits, 1);
        a = first_temp;
        b = second_temp;
    }
    __ mrs(v::x30, v::NZCV);  // Preserve the actual guest/packed-mode host NZCV.
    __ fcmp(bits == 32 ? S(a) : D(a), bits == 32 ? S(b) : D(b));
    __ cset(W(destination), v::lt);  // CSET Wdestination,LT: less or unordered.
    __ cset(v::w0,
            v::vs);          // CSET W0,VS: unordered (destination may be spill scratch16).
    __ cset(v::w17, v::eq);  // CSET W17,EQ.
    __ orr(v::w17, v::w17, Operand(v::w0));  // Equal or unordered.
    __ orr(W(destination), W(destination), Operand(v::w0, v::LSL, 1));
    __ orr(W(destination), W(destination), Operand(v::w17, v::LSL, 2));
    __ msr(v::NZCV, v::x30);  // MSR NZCV,x30: publishing flags is a separate op.
}

void Writer::HandleVecFCvtPackedFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto input = VectorValue(cursor.Take(), 0);
    const auto kind = IntegerImmediate(cursor.Take());
    const auto copy = VectorTemporary();
    if (kind > 7) Reject(CompileError::UnsupportedOperand);
    if (kind == 0) {
        __ scvtf(Lanes(ctx.destination, 32), Lanes(input, 32));
    } else if (kind == 1) {
        __ sshll(Lanes(ctx.destination, 64), Lanes(input, 32, 64), 0);
        __ scvtf(Lanes(ctx.destination, 64), Lanes(ctx.destination, 64));
    } else {
        // Scalar conversion scratch v0 must not overwrite a spilled input,
        // nor may a result tied to input erase its next lane.
        VectorMove(copy, input);
        __ eor(Lanes(ctx.destination, 8), Lanes(ctx.destination, 8), Lanes(ctx.destination, 8));
        const unsigned input_bits = kind == 2 || kind == 3 || kind == 6 ? 32 : 64;
        const unsigned output_bits = kind == 6 ? 64 : 32;
        const unsigned lanes = kind == 2 || kind == 3 ? 4 : 2;
        for (unsigned i = 0; i < lanes; ++i) {
            __ umov(R(16, input_bits), Lanes(copy, input_bits), i);
            if (kind <= 5)
                ConvertFloatToInteger(30, 16, input_bits, 32, kind == 3 || kind == 5);
            else
                ConvertScalarFloat(30, 16, input_bits);
            __ ins(Lanes(ctx.destination, output_bits), i, R(30, output_bits));
        }
    }
}

void Writer::HandleVecFCmpMaskFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto bits = IntegerImmediate(cursor.Take()), relations = IntegerImmediate(cursor.Take());
    const auto scalar = IntegerImmediate(cursor.Take());
    if ((bits != 32 && bits != 64) || relations > 15 || scalar > 1)
        Reject(CompileError::UnsupportedOperand);
    const auto first = VectorTemporary(), second = VectorTemporary(true);
    const bool flush = options.mxcsr_flush_offset != UINT32_MAX &&
                       (options.mxcsr_flush_mode == UINT8_MAX || (options.mxcsr_flush_mode & 1u));
    unsigned a = left, b = right, temporary = first;
    const unsigned destination = scalar ? 0 : ctx.destination;
    if (scalar || flush) {
        VectorMove(first, left);
        VectorMove(second, right);
        if (flush) {
            LoadMxcsrFlushControl();
            FlushScalarDenormal(first, 6, 0, bits, scalar ? 1 : 128 / bits);
            FlushScalarDenormal(second, 6, 0, bits, scalar ? 1 : 128 / bits);
        }
        a = first;
        b = second;
        temporary = scalar ? 1 : 0;
    }
    const bool invert = relations & 8;
    const auto ordered = static_cast<unsigned>(invert ? ~relations & 7 : relations);
    if (!ordered) {
        if (invert)
            __ cmeq(Lanes(destination, 64), Lanes(destination, 64), Lanes(destination, 64));
        else
            __ eor(Lanes(destination, 8), Lanes(destination, 8), Lanes(destination, 8));
    } else {
        if (ordered == 5 || ordered == 7) {
            if (ordered == 5)
                __ fcmgt(Lanes(temporary, bits), Lanes(a, bits), Lanes(b, bits));
            else
                __ fcmge(Lanes(temporary, bits), Lanes(a, bits), Lanes(b, bits));
            __ fcmgt(Lanes(destination, bits), Lanes(b, bits), Lanes(a, bits));
            __ orr(Lanes(destination, 8), Lanes(temporary, 8), Lanes(destination, 8));
        } else {
            if (ordered == 1 || ordered == 3) std::swap(a, b);
            if (ordered == 2)
                __ fcmeq(Lanes(destination, bits), Lanes(a, bits), Lanes(b, bits));
            else if (ordered == 3 || ordered == 6)
                __ fcmge(Lanes(destination, bits), Lanes(a, bits), Lanes(b, bits));
            else
                __ fcmgt(Lanes(destination, bits), Lanes(a, bits), Lanes(b, bits));
        }
        if (invert) __ not_(Lanes(destination, 8), Lanes(destination, 8));
    }
    if (scalar) {
        VectorMove(ctx.destination, first);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(destination, bits), 0);
    }
}

void Writer::HandleVecFMinMaxFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& c = ctx.cursor;
    const auto left = VectorValue(c.Take(), 0), right = VectorValue(c.Take(), 1);
    const auto bits = IntegerImmediate(c.Take()), maximum = IntegerImmediate(c.Take()),
               scalar = IntegerImmediate(c.Take());
    if ((bits != 32 && bits != 64) || maximum > 1 || scalar > 1)
        Reject(CompileError::UnsupportedOperand);
    unsigned a = left, b = right, result = VectorTemporary();
    if (options.mxcsr_flush_offset != UINT32_MAX &&
        (options.mxcsr_flush_mode == UINT8_MAX || (options.mxcsr_flush_mode & 1u))) {
        a = VectorTemporary();
        b = VectorTemporary(true);
        result = 0;
        VectorMove(a, left);
        VectorMove(b, right);
        LoadMxcsrFlushControl();
        FlushScalarDenormal(a, 6, 0, bits, scalar ? 1 : 128 / bits);
        FlushScalarDenormal(b, 6, 0, bits, scalar ? 1 : 128 / bits);
    }
    // Strict comparison selects source 2 for equal values and unordered,
    // retaining that operand's zero sign and even its signaling NaN payload.
    __ fcmgt(Lanes(result, bits), Lanes(maximum ? a : b, bits), Lanes(maximum ? b : a, bits));
    __ bsl(Lanes(result, 8), Lanes(a, 8), Lanes(b, 8));
    if (scalar) {
        VectorMove(ctx.destination, a);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(result, bits), 0);
    } else
        VectorMove(ctx.destination, result);
}

void Writer::HandleVecFUnaryFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& c = ctx.cursor;
    const auto merge = VectorValue(c.Take(), 0), input = VectorValue(c.Take(), 1);
    const auto bits = IntegerImmediate(c.Take()), kind = IntegerImmediate(c.Take()),
               scalar = IntegerImmediate(c.Take());
    if ((bits != 32 && bits != 64) || kind > 2 || scalar > 1 || (kind && bits != 32))
        Reject(CompileError::UnsupportedOperand);
    const auto value = VectorTemporary(), one = VectorTemporary(true);
    const unsigned count = scalar ? 1 : 128 / bits;
    VectorMove(value, input);
    const auto flush_estimate = [&] {
        // RCP/RSQRT treat denormals as signed zero, independently of MXCSR.
        for (unsigned lane = 0; lane < count; ++lane) {
            __ umov(v::w16, Lanes(value, 32), lane);
            Extract(17, 16, 23, 8);
            const auto normal = masm.Here();
            __ cbnz(v::x17, std::int64_t{0});
            __ and_(v::w16, v::w16, Operand(UINT32_C(0x80000000)));
            __ ins(Lanes(value, 32), lane, v::w16);
            masm.Patch(normal, masm.Here());
        }
    };
    if (kind)
        flush_estimate();
    else if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        LoadMxcsrFlushControl();
        FlushScalarDenormal(value, 6, 0, bits, count);
    }
    if (scalar) {
        const auto v = bits == 32 ? S(value) : D(value);
        if (kind != 1) __ fsqrt(v, v);
        if (kind) {
            __ fmov(S(one), 1.0);
            __ fdiv(S(value), S(one), S(value));
        }
    } else {
        if (kind != 1) __ fsqrt(Lanes(value, bits), Lanes(value, bits));
        if (kind) {
            Immediate(17, UINT32_C(0x3f800000));
            __ dup(Lanes(one, 32), v::w17);
            __ fdiv(Lanes(value, 32), Lanes(one, 32), Lanes(value, 32));
        }
    }
    if (kind)
        flush_estimate();
    else if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0)
        FlushScalarDenormal(value, 15, 0, bits, count);
    if (scalar) {
        VectorMove(ctx.destination, merge);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(value, bits), 0);
    } else
        VectorMove(ctx.destination, value);
}

void Writer::HandleVecFRoundIntFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& c = ctx.cursor;
    const auto merge = VectorValue(c.Take(), 0), input = VectorValue(c.Take(), 1);
    const auto bits = IntegerImmediate(c.Take()), mode = IntegerImmediate(c.Take()),
               scalar = IntegerImmediate(c.Take());
    if ((bits != 32 && bits != 64) || mode > 3 || scalar > 1)
        Reject(CompileError::UnsupportedOperand);
    const auto value = VectorTemporary();
    VectorMove(value, input);
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        LoadMxcsrFlushControl();
        FlushScalarDenormal(value, 6, 0, bits, scalar ? 1 : 128 / bits);
    }
    const auto fp = scalar ? (bits == 32 ? S(value) : D(value)) : Lanes(value, bits);
    if (mode == 0)
        __ frintn(fp, fp);
    else if (mode == 1)
        __ frintm(fp, fp);
    else if (mode == 2)
        __ frintp(fp, fp);
    else
        __ frintz(fp, fp);
    if (scalar) {
        VectorMove(ctx.destination, merge);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(value, bits), 0);
    } else
        VectorMove(ctx.destination, value);
}

void Writer::HandleVecFMulAddFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& c = ctx.cursor;
    const auto first = VectorTemporary(), result = VectorTemporary(true);
    // A third spilled operand also uses v0. Capture each original input
    // before the next load, keeping exactly the normal four SIMD scratches.
    const auto original_a = VectorValue(c.Take(), 0);
    VectorMove(first, original_a);
    const auto original_b = VectorValue(c.Take(), 1);
    VectorMove(1, original_b);
    const auto original_c = VectorValue(c.Take(), 0);
    VectorMove(0, original_c);
    const auto bits = IntegerImmediate(c.Take()), flags = IntegerImmediate(c.Take());
    if ((bits != 32 && bits != 64) || flags > 31) Reject(CompileError::UnsupportedOperand);
    const bool scalar = flags & 4;
    const unsigned count = scalar ? 1 : 128 / bits;
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        LoadMxcsrFlushControl();
        FlushScalarDenormal(first, 6, 0, bits, count);
        FlushScalarDenormal(1, 6, 0, bits, count);
        FlushScalarDenormal(0, 6, 0, bits, count);
    }
    bool negative_add = false;
    for (unsigned i = 0; i < count; ++i)
        negative_add |= bool(flags & 2) ^ (bool(flags & 8) && ((i & 1) == ((flags >> 4) & 1)));
    if (negative_add) {
        // A sign modifier must not negate the sign of a selected NaN.
        __ fcmeq(Lanes(result, bits), Lanes(0, bits), Lanes(0, bits));
        __ shl(Lanes(result, bits), Lanes(result, bits), bits - 1);
        for (unsigned i = 0; i < 128 / bits; ++i) {
            const bool negate = i < count && (bool(flags & 2) ^
                                              (bool(flags & 8) && ((i & 1) == ((flags >> 4) & 1))));
            if (!negate) __ ins(Lanes(result, bits), i, R(31, bits));
        }
        __ eor(Lanes(0, 8), Lanes(0, 8), Lanes(result, 8));
    }
    VectorMove(result, 0);
    if (flags & 1)
        __ fmls(Lanes(result, bits), Lanes(first, bits), Lanes(1, bits));
    else
        __ fmla(Lanes(result, bits), Lanes(first, bits), Lanes(1, bits));
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0)
        FlushScalarDenormal(result, 15, 0, bits, count);
    // Arithmetic operand priority is a, then b, then c. The fused host
    // operation already supplies c when it is the only NaN. Override b and
    // then a without arithmetic on their payloads or any NZCV modification.
    const unsigned quiet_bit = bits == 32 ? 22 : 51;
    for (const auto input : {1u, first}) {
        __ fcmeq(Lanes(0, bits), Lanes(input, bits), Lanes(input, bits));
        __ not_(Lanes(0, 8), Lanes(0, 8));
        if (scalar) {
            __ umov(R(16, bits), Lanes(input, bits), 0);
            __ orr(R(16, bits), R(16, bits), Operand(u64{1} << quiet_bit));
            __ ins(Lanes(input, bits), 0, R(16, bits));
        } else {
            __ ushr(Lanes(0, bits), Lanes(0, bits), bits - 1);
            __ shl(Lanes(0, bits), Lanes(0, bits), quiet_bit);
            __ orr(Lanes(input, 8), Lanes(input, 8), Lanes(0, 8));
            __ fcmeq(Lanes(0, bits), Lanes(input, bits), Lanes(input, bits));
            __ not_(Lanes(0, 8), Lanes(0, 8));
        }
        __ bsl(Lanes(0, 8), Lanes(input, 8), Lanes(result, 8));
        VectorMove(result, 0);
    }
    if (scalar) {
        VectorMove(ctx.destination, first);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(result, bits), 0);
    } else
        VectorMove(ctx.destination, result);
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
