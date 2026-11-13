#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {

void Writer::FlushScalarDenormal(unsigned value,
                                 unsigned control_bit,
                                 unsigned lane,
                                 unsigned lane_bits,
                                 unsigned lane_count) {
    // Known software controls belong to this whole native unit. Otherwise
    // x0 holds the actual raw MXCSR controls. Neither branch changes NZCV.
    u32 disabled = UINT32_MAX;
    if (options.mxcsr_flush_offset == UINT32_MAX) {
        if (!options.native_fp_environment || control_bit != 6) return;
        // An AFP comparison consumed FIZ, but a subsequent integer bitselect
        // still contains the original input bits. Normalize only that value.
        __ mrs(v::x0, v::FPCR);
        disabled = masm.Here();
        __ tbz(v::w0, 0, std::int64_t{0});
    } else if (options.mxcsr_flush_mode != UINT8_MAX) {
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
        __ lsl(R(17, lane_bits), R(16, lane_bits), 1);
        const auto zero = masm.Here();
        __ cbz(R(17, lane_bits), std::int64_t{0});
        if (control_bit == 15) {
            __ mrs(v::x17, v::FPSR);
            __ orr(v::x17, v::x17, Operand(u64{0x18}));  // Flushing a tiny result is inexact.
            __ msr(v::FPSR, v::x17);
        }
        __ and_(v::x16,
                v::x16,
                Operand(u64{1} << (lane_bits - 1)));                 // Keep only the IEEE sign bit.
        __ ins(Lanes(value, lane_bits), i, single ? W(16) : X(16));  // INS preserves other lanes.
        masm.Patch(normal, masm.Here());
        masm.Patch(zero, masm.Here());
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
    LoadMxcsrFlushControl();
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0)
        FlushScalarDenormal(0, 6, 0, float_bits, 1);
    // Integer conversions do not report DE. Preserve older sticky flags while
    // keeping only this conversion's IE/PE, including the invalid-priority rule.
    __ mrs(v::x0, v::FPSR);
    if (!truncate) {
        __ frintx(fp, fp);
    }
    __ fmov(R(16, float_bits), fp);
    Extract(17, 16, float_bits == 32 ? 23 : 52, float_bits == 32 ? 8 : 11);
    __ sub(v::x17, v::x17, Operand((float_bits == 32 ? 127u : 1023u) + integer_bits - 1));
    const auto below = masm.Here();
    __ tbnz(v::x17, 63, std::int64_t{0});
    const auto above = masm.Here();
    __ cbnz(v::x17, std::int64_t{0});
    const auto positive_edge = masm.Here();
    __ tbz(R(16, float_bits), float_bits - 1, std::int64_t{0});
    const unsigned fraction = float_bits == 32 ? 23 : 52;
    // Truncation admits the negative interval (minInt-1,minInt], not only
    // exactly minInt. Rounded conversions have already removed the fraction.
    const unsigned window = truncate && fraction >= integer_bits ? fraction - integer_bits + 1 : 0;
    Extract(17, 16, window, fraction - window);
    const auto negative_edge = masm.Here();
    __ cbnz(v::x17, std::int64_t{0});
    masm.Patch(below, masm.Here());
    __ fcvtzs(R(destination, integer_bits == 64 ? 64 : 32), fp);
    __ mrs(v::x17, v::FPSR);
    __ and_(v::x17, v::x17, Operand(u64{0x1f}));
    __ and_(v::x17, v::x17, Operand(~u64{0xe}));
    __ orr(v::x17, v::x17, Operand(v::x0));
    __ msr(v::FPSR, v::x17);
    const auto converted = masm.Here();
    __ b(std::int64_t{0});
    masm.Patch(above, masm.Here());
    masm.Patch(positive_edge, masm.Here());
    masm.Patch(negative_edge, masm.Here());
    __ orr(v::x0, v::x0, Operand(u64{1}));
    __ msr(v::FPSR, v::x0);
    Immediate(destination, u64{1} << (integer_bits - 1));
    masm.Patch(converted, masm.Here());
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
    LoadMxcsrFlushControl();
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0)
        FlushScalarDenormal(0, 6, 0, source_bits, 1);
    RecordDenormalInput(0, source_bits, 1);
    __ fcvt(result, input);
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0)
        FlushScalarDenormal(0, 15, 0, result_bits, 1);
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
    const auto left_cell = cursor.Take(), right_cell = cursor.Take();
    const auto left = VectorValue(left_cell, 0), right = VectorValue(right_cell, 1);
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
    unsigned a = left, b = right;
    const auto lanes = static_cast<unsigned>(128 / bits);
    const bool flush = options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0;
    LoadMxcsrFlushControl();
    if (flush) {
        FlushDenormalBatch();
        VectorMove(first_temp, left);
        VectorMove(second_temp, right);
        FlushScalarDenormal(first_temp, 6, 0, static_cast<unsigned>(bits), lanes);
        FlushScalarDenormal(second_temp, 6, 0, static_cast<unsigned>(bits), lanes);
        a = first_temp;
        b = second_temp;
    }
    const auto denominator = operation.opcode == OpCode::VecFDiv ? b : UINT32_MAX;
    const bool skip_left = ProvenNoDenormal(left_cell, bits, lanes);
    const bool skip_right = ProvenNoDenormal(right_cell, bits, lanes);
    if (!flush) {
        QueueDenormalInputs(
                a, b, bits, lanes, denominator, skip_left, skip_right, u32{1} << destination);
    } else {
        RecordDenormalInputs(a, b, bits, lanes, denominator, skip_left, skip_right);
    }
    emit(destination, a, b);
    if (flush)
        FlushScalarDenormal(destination, 15, 0, static_cast<unsigned>(bits), lanes);
}

void Writer::HandleVecFAddScalar64Family(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto left_cell = cursor.Take(), right_cell = cursor.Take();
    const auto left = VectorValue(left_cell, 0), right = VectorValue(right_cell, 1);
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
    const bool skip_left = ProvenNoDenormal(left_cell, bits, 1);
    const bool skip_right = ProvenNoDenormal(right_cell, bits, 1);
    LoadMxcsrFlushControl();
    const bool divide = opcode == OpCode::VecFDivScalar32 || opcode == OpCode::VecFDivScalar64;
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        VectorMove(first, left);
        VectorMove(second, right);
        FlushScalarDenormal(first, 6, 0, bits, 1);
        FlushScalarDenormal(second, 6, 0, bits, 1);
        RecordDenormalInputs(
                first, second, bits, 1, divide ? second : UINT32_MAX, skip_left, skip_right);
        emit(first, second);
        FlushScalarDenormal(first, 15, 0, bits, 1);
    } else {
        RecordDenormalInputs(
                left, right, bits, 1, divide ? right : UINT32_MAX, skip_left, skip_right);
        emit(left, right);
    }
    VectorMove(ctx.destination, left);
    __ ins(Lanes(ctx.destination, bits), 0, Lanes(first, bits), 0);
}

void Writer::HandleArmScalarFPFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto kind = IntegerImmediate(cursor.Take()), bits = IntegerImmediate(cursor.Take());
    if ((bits != 32 && bits != 64) || kind > 3 || options.fpsr_offset == UINT32_MAX)
        Reject(CompileError::UnsupportedOperand);
    const auto result = VectorTemporary();
    const auto out = bits == 32 ? S(result) : D(result);
    const auto lhs = bits == 32 ? S(left) : D(left), rhs = bits == 32 ? S(right) : D(right);
    if (kind == 0)
        __ fadd(out, lhs, rhs);
    else if (kind == 1)
        __ fsub(out, lhs, rhs);
    else if (kind == 2)
        __ fmul(out, lhs, rhs);
    else
        __ fdiv(out, lhs, rhs);
    VectorMove(ctx.destination, result);
}

void Writer::HandleArmScalarFMAFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1),
               addend = VectorValue(cursor.Take(), 2);
    const auto mode = IntegerImmediate(cursor.Take());
    if (mode > 7 || options.fpsr_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    const auto kind = static_cast<unsigned>(mode & 3u);
    const unsigned bits = (mode & 4u) ? 64u : 32u;
    const auto result = VectorTemporary();
    const auto out = bits == 32 ? S(result) : D(result);
    const auto lhs = bits == 32 ? S(left) : D(left), rhs = bits == 32 ? S(right) : D(right),
               acc = bits == 32 ? S(addend) : D(addend);
    if (kind == 0)
        __ fmadd(out, lhs, rhs, acc);
    else if (kind == 1)
        __ fmsub(out, lhs, rhs, acc);
    else if (kind == 2)
        __ fnmadd(out, lhs, rhs, acc);
    else
        __ fnmsub(out, lhs, rhs, acc);
    VectorMove(ctx.destination, result);
}

void Writer::PublishGuestFPSR() {
    FlushDenormalBatch();
    // The guest FPCR/FPSR are installed for the whole native unit, so the
    // hardware flags are architectural. Publish the cumulative exception
    // bits after every operation; helpers and MRS read the uniform copy.
    Memory(true, 32, 16, 19, options.fpsr_offset / 4);
    __ mrs(v::x17, v::FPSR);
    __ ubfx(v::w17, v::x17, 0, 8);
    __ orr(v::w17, v::w17, Operand(v::w16));
    Memory(false, 32, 17, 19, options.fpsr_offset / 4);
}

void Writer::HandleVecFCmpFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto first_temp = VectorTemporary();
    const auto second_temp = VectorTemporary(true);
    const auto left_cell = cursor.Take(), right_cell = cursor.Take();
    const auto left = VectorValue(left_cell, 0), right = VectorValue(right_cell, 1);
    const auto encoded = IntegerImmediate(cursor.Take());
    const auto bits = encoded & ~u64{128};
    if (bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    unsigned a = left, b = right;
    LoadMxcsrFlushControl();
    if (options.mxcsr_flush_offset != UINT32_MAX &&
        (options.mxcsr_flush_mode == UINT8_MAX || (options.mxcsr_flush_mode & 1u))) {
        VectorMove(first_temp, left);
        VectorMove(second_temp, right);
        FlushScalarDenormal(first_temp, 6, 0, bits, 1);
        FlushScalarDenormal(second_temp, 6, 0, bits, 1);
        a = first_temp;
        b = second_temp;
    }
    RecordDenormalInputs(a,
                         b,
                         bits,
                         1,
                         UINT32_MAX,
                         ProvenNoDenormal(left_cell, bits, 1),
                         ProvenNoDenormal(right_cell, bits, 1));
    __ mrs(v::x30, v::NZCV);  // Preserve the actual guest/packed-mode host NZCV.
    if (encoded & 128)
        __ fcmpe(bits == 32 ? S(a) : D(a), bits == 32 ? S(b) : D(b));
    else
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
        const auto result = ctx.destination == 0 ? VectorTemporary(true) : ctx.destination;
        __ eor(Lanes(result, 8), Lanes(result, 8), Lanes(result, 8));
        const unsigned input_bits = kind == 2 || kind == 3 || kind == 6 ? 32 : 64;
        const unsigned output_bits = kind == 6 ? 64 : 32;
        const unsigned lanes = kind == 2 || kind == 3 ? 4 : 2;
        for (unsigned i = 0; i < lanes; ++i) {
            __ umov(R(16, input_bits), Lanes(copy, input_bits), i);
            if (kind <= 5)
                ConvertFloatToInteger(30, 16, input_bits, 32, kind == 3 || kind == 5);
            else
                ConvertScalarFloat(30, 16, input_bits);
            __ ins(Lanes(result, output_bits), i, R(30, output_bits));
        }
        VectorMove(ctx.destination, result);
    }
}

void Writer::HandleVecFCmpMaskFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto left_cell = cursor.Take(), right_cell = cursor.Take();
    const auto left = VectorValue(left_cell, 0), right = VectorValue(right_cell, 1);
    const auto bits = IntegerImmediate(cursor.Take()), encoded = IntegerImmediate(cursor.Take());
    const auto relations = encoded & 15;
    const auto scalar = IntegerImmediate(cursor.Take());
    if ((bits != 32 && bits != 64) || encoded > 31 || scalar > 1)
        Reject(CompileError::UnsupportedOperand);
    const auto first = VectorTemporary(), second = VectorTemporary(true);
    const bool flush = options.mxcsr_flush_offset != UINT32_MAX &&
                       (options.mxcsr_flush_mode == UINT8_MAX || (options.mxcsr_flush_mode & 1u));
    const bool invert = relations & 8;
    const auto ordered = static_cast<unsigned>(invert ? ~relations & 7 : relations);
    // FCMEQ is quiet, while FCMGT/FCMGE signal on every NaN. Preserve their
    // accrued exceptions when that exactly matches the guest predicate.
    const bool native_status = ordered && bool(encoded & 16) == (ordered != 2);
    const bool can_batch = !scalar && !flush && native_status;
    if (!can_batch) FlushDenormalBatch();
    unsigned a = left, b = right;
    const unsigned destination = scalar ? (ctx.destination == 0 ? 1 : 0) : ctx.destination;
    LoadMxcsrFlushControl();
    if (scalar || flush) {
        VectorMove(first, left);
        VectorMove(second, right);
        if (flush) {
            FlushScalarDenormal(first, 6, 0, bits, scalar ? 1 : 128 / bits);
            FlushScalarDenormal(second, 6, 0, bits, scalar ? 1 : 128 / bits);
        }
        a = first;
        b = second;
    }
    unsigned temporary{};
    for (const auto candidate : {first, second, 0u, 1u})
        if (candidate != a && candidate != b && candidate != destination) {
            temporary = candidate;
            break;
        }
    const unsigned count = scalar ? 1 : 128 / bits;
    const bool skip_left = ProvenNoDenormal(left_cell, bits, count);
    const bool skip_right = ProvenNoDenormal(right_cell, bits, count);
    if (can_batch) {
        QueueDenormalInputs(
                a, b, bits, count, UINT32_MAX, skip_left, skip_right, u32{1} << destination);
    } else {
        RecordDenormalInputs(a, b, bits, count, UINT32_MAX, skip_left, skip_right);
    }
    if (!native_status) {
        RecordNaNInput(a, bits, count, encoded & 16);
        RecordNaNInput(b, bits, count, encoded & 16);
        // Quiet/compound predicates may need a signaling host comparison.
        // Only the explicit classification may then publish IE.
        if (ordered) __ mrs(v::x30, v::FPSR);
    }
    const auto fp = [&](unsigned reg) {
        return scalar ? (bits == 32 ? S(reg) : D(reg)) : Lanes(reg, bits);
    };
    if (!ordered) {
        if (invert)
            __ cmeq(Lanes(destination, 64), Lanes(destination, 64), Lanes(destination, 64));
        else
            __ eor(Lanes(destination, 8), Lanes(destination, 8), Lanes(destination, 8));
    } else {
        if (ordered == 5 || ordered == 7) {
            if (ordered == 5)
                __ fcmgt(fp(temporary), fp(a), fp(b));
            else
                __ fcmge(fp(temporary), fp(a), fp(b));
            __ fcmgt(fp(destination), fp(b), fp(a));
            __ orr(Lanes(destination, 8), Lanes(temporary, 8), Lanes(destination, 8));
        } else {
            if (ordered == 1 || ordered == 3) std::swap(a, b);
            if (ordered == 2)
                __ fcmeq(fp(destination), fp(a), fp(b));
            else if (ordered == 3 || ordered == 6)
                __ fcmge(fp(destination), fp(a), fp(b));
            else
                __ fcmgt(fp(destination), fp(a), fp(b));
        }
        if (invert) __ not_(Lanes(destination, 8), Lanes(destination, 8));
    }
    if (!native_status && ordered) __ msr(v::FPSR, v::x30);
    if (scalar) {
        VectorMove(ctx.destination, first);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(destination, bits), 0);
    }
}

void Writer::HandleVecFMinMaxFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& c = ctx.cursor;
    const auto left_cell = c.Take(), right_cell = c.Take();
    const auto left = VectorValue(left_cell, 0), right = VectorValue(right_cell, 1);
    const auto bits = IntegerImmediate(c.Take()), maximum = IntegerImmediate(c.Take()),
               scalar = IntegerImmediate(c.Take());
    if ((bits != 32 && bits != 64) || maximum > 1 || scalar > 1)
        Reject(CompileError::UnsupportedOperand);
    const unsigned count = scalar ? 1 : 128 / bits;
    const bool flush = options.mxcsr_flush_offset != UINT32_MAX &&
                       (options.mxcsr_flush_mode == UINT8_MAX || (options.mxcsr_flush_mode & 1u));
    const bool afp_post_flush =
            options.mxcsr_flush_offset == UINT32_MAX && options.native_fp_environment;
    const bool can_batch = !scalar && !flush && !afp_post_flush;
    if (!can_batch) FlushDenormalBatch();
    unsigned a = left, b = right, result = VectorTemporary();
    LoadMxcsrFlushControl();
    if (flush) {
        a = VectorTemporary();
        b = VectorTemporary(true);
        result = ctx.destination == 0 ? 1 : 0;
        VectorMove(a, left);
        VectorMove(b, right);
        FlushScalarDenormal(a, 6, 0, bits, count);
        FlushScalarDenormal(b, 6, 0, bits, count);
    }
    const bool skip_left = ProvenNoDenormal(left_cell, bits, count);
    const bool skip_right = ProvenNoDenormal(right_cell, bits, count);
    if (can_batch) {
        QueueDenormalInputs(
                a, b, bits, count, UINT32_MAX, skip_left, skip_right, u32{1} << ctx.destination);
    } else {
        RecordDenormalInputs(a, b, bits, count, UINT32_MAX, skip_left, skip_right);
    }
    // Strict comparison selects source 2 for equal values and unordered,
    // retaining that operand's zero sign and even its signaling NaN payload.
    const auto fp = [&](unsigned reg) {
        return scalar ? (bits == 32 ? S(reg) : D(reg)) : Lanes(reg, bits);
    };
    __ fcmgt(fp(result), fp(maximum ? a : b), fp(maximum ? b : a));
    __ bsl(Lanes(result, 8), Lanes(a, 8), Lanes(b, 8));
    if (options.mxcsr_flush_offset == UINT32_MAX && options.native_fp_environment)
        FlushScalarDenormal(result, 6, 0, bits, count);
    if (scalar) {
        VectorMove(ctx.destination, a);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(result, bits), 0);
    } else
        VectorMove(ctx.destination, result);
}

void Writer::HandleVecFUnaryFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& c = ctx.cursor;
    const auto merge = VectorValue(c.Take(), 0);
    const auto input_cell = c.Take();
    const auto input = VectorValue(input_cell, 1);
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
    else {
        LoadMxcsrFlushControl();
        if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0)
            FlushScalarDenormal(value, 6, 0, bits, count);
        if (!ProvenNoDenormal(input_cell, bits, count)) RecordDenormalInput(value, bits, count);
    }
    if (kind) __ mrs(v::x30, v::FPSR);  // RCP/RSQRT do not publish SIMD FP exceptions.
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
    if (kind) __ msr(v::FPSR, v::x30);
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
    if ((bits != 32 && bits != 64) || (mode & ~u64{11}) || scalar > 1)
        Reject(CompileError::UnsupportedOperand);
    const auto value = VectorTemporary();
    VectorMove(value, input);
    LoadMxcsrFlushControl();
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        FlushScalarDenormal(value, 6, 0, bits, scalar ? 1 : 128 / bits);
    }
    __ mrs(v::x0, v::FPSR);
    const auto fp = scalar ? (bits == 32 ? S(value) : D(value)) : Lanes(value, bits);
    const auto rounding = mode & 3;
    if (!(mode & 8)) {
        // FRINTX reports precision under the active FPCR rounding mode.
        constexpr unsigned arm_rounding[]{0, 2, 1, 3};
        __ mrs(v::x16, v::FPCR);
        __ and_(v::x17, v::x16, Operand(~(u64{3} << 22)));
        if (rounding) __ orr(v::x17, v::x17, Operand(u64{arm_rounding[rounding]} << 22));
        __ msr(v::FPCR, v::x17);
        __ frintx(fp, fp);
        __ msr(v::FPCR, v::x16);
    } else if (rounding == 0)
        __ frintn(fp, fp);
    else if (rounding == 1)
        __ frintm(fp, fp);
    else if (rounding == 2)
        __ frintp(fp, fp);
    else
        __ frintz(fp, fp);
    __ mrs(v::x17, v::FPSR);
    __ and_(v::x17, v::x17, Operand(mode & 8 ? u64{1} : u64{0x1f}));
    if (!(mode & 8)) __ and_(v::x17, v::x17, Operand(~u64{0xe}));
    __ orr(v::x17, v::x17, Operand(v::x0));
    __ msr(v::FPSR, v::x17);  // ROUND reports only IE/PE, never DE.
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
    LoadMxcsrFlushControl();
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0) {
        FlushScalarDenormal(first, 6, 0, bits, count);
        FlushScalarDenormal(1, 6, 0, bits, count);
        FlushScalarDenormal(0, 6, 0, bits, count);
    }
    RecordDenormalInput(first, bits, count, {first, 1u, 0u}, UINT32_MAX, flags);
    bool negative_add = false;
    for (unsigned i = 0; i < count; ++i)
        negative_add |= bool(flags & 2) ^ (bool(flags & 8) && ((i & 1) == ((flags >> 4) & 1)));
    if (negative_add) {
        // A sign modifier must not negate the sign of a selected NaN.
        __ mrs(v::x30, v::FPSR);
        __ fcmeq(Lanes(result, bits), Lanes(0, bits), Lanes(0, bits));
        __ shl(Lanes(result, bits), Lanes(result, bits), bits - 1);
        for (unsigned i = 0; i < 128 / bits; ++i) {
            const bool negate = i < count && (bool(flags & 2) ^
                                              (bool(flags & 8) && ((i & 1) == ((flags >> 4) & 1))));
            if (!negate) __ ins(Lanes(result, bits), i, R(31, bits));
        }
        __ eor(Lanes(0, 8), Lanes(0, 8), Lanes(result, 8));
        __ msr(v::FPSR, v::x30);
    }
    VectorMove(result, 0);
    if (scalar) {
        const auto fp = [&](unsigned reg) { return bits == 32 ? S(reg) : D(reg); };
        if (flags & 1)
            __ fnmsub(fp(result), fp(first), fp(1), fp(0));
        else
            __ fmadd(fp(result), fp(first), fp(1), fp(0));
    } else if (flags & 1)
        __ fmls(Lanes(result, bits), Lanes(first, bits), Lanes(1, bits));
    else
        __ fmla(Lanes(result, bits), Lanes(first, bits), Lanes(1, bits));
    if (options.mxcsr_flush_offset != UINT32_MAX && options.mxcsr_flush_mode != 0)
        FlushScalarDenormal(result, 15, 0, bits, count);
    // Arithmetic operand priority is a, then b, then c. The fused host
    // operation already supplies c when it is the only NaN. Override b and
    // then a without arithmetic on their payloads or any NZCV modification.
    const unsigned quiet_bit = bits == 32 ? 22 : 51;
    __ mrs(v::x30, v::FPSR);
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
    __ msr(v::FPSR, v::x30);
    if (scalar) {
        VectorMove(ctx.destination, first);
        __ ins(Lanes(ctx.destination, bits), 0, Lanes(result, bits), 0);
    } else
        VectorMove(ctx.destination, result);
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
