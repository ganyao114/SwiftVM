#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {
void Writer::HandleVecSharedZeroFamily(OpContext& ctx) {
    if (ctx.home.spilled && ctx.home.stack_offset == UINT32_MAX) return;
    const auto destination = ctx.destination;
    __ eor(Lanes(destination, 8), Lanes(destination, 8), Lanes(destination, 8));
}

void Writer::HandleVecLoadConstFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto low = IntegerImmediate(cursor.Take()), high = IntegerImmediate(cursor.Take());
    if (ctx.home.spilled && ctx.home.stack_offset == UINT32_MAX) return;
    if (!low && !high) {
        __ eor(Lanes(destination, 8), Lanes(destination, 8), Lanes(destination, 8));
        return;
    }
    if (low == UINT64_MAX && high == UINT64_MAX) {
        __ cmeq(Lanes(destination, 64), Lanes(destination, 64), Lanes(destination, 64));
        return;
    }
    Immediate(16, low);
    if (low == high) {
        __ dup(Lanes(destination, 64), v::x16);
        return;
    }
    __ fmov(D(destination), v::x16);
    if (!high) return;
    Immediate(17, high);
    __ ins(Lanes(destination, 64), 1, v::x17);
}

void Writer::HandleVecDup64Family(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto input = Read(cursor.Take(), 16);
    __ dup(Lanes(destination, 64), X(input));
}

void Writer::HandleVecScalarFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    const auto input_cell = ctx.cursor.Take();
    const auto bits = Width(View(input_cell));
    const auto input = Read(input_cell, 16, 64, false, bits == 32 ? 32u : 64u);
    if (bits <= 32)
        __ fmov(S(ctx.destination), W(input));
    else
        __ fmov(D(ctx.destination), X(input));
}

void Writer::HandleVecMinFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto bits = IntegerImmediate(cursor.Take()), sign = IntegerImmediate(cursor.Take());
    LaneSize(bits);
    if (sign > 1) Reject(CompileError::UnsupportedOperand);
    const bool maximum = ctx.operation.opcode == OpCode::VecMax;
    if (bits == 64) {
        const auto temp = VectorTemporary();
        const auto a = maximum ? left : right, b = maximum ? right : left;
        if (sign)
            __ cmgt(Lanes(temp, 64), Lanes(a, 64), Lanes(b, 64));
        else
            __ cmhi(Lanes(temp, 64), Lanes(a, 64), Lanes(b, 64));
        __ bsl(Lanes(temp, 8), Lanes(left, 8), Lanes(right, 8));
        VectorMove(ctx.destination, temp);
    } else if (sign) {
        if (maximum)
            __ smax(Lanes(ctx.destination, bits), Lanes(left, bits), Lanes(right, bits));
        else
            __ smin(Lanes(ctx.destination, bits), Lanes(left, bits), Lanes(right, bits));
    } else {
        if (maximum)
            __ umax(Lanes(ctx.destination, bits), Lanes(left, bits), Lanes(right, bits));
        else
            __ umin(Lanes(ctx.destination, bits), Lanes(left, bits), Lanes(right, bits));
    }
}

void Writer::HandleVecMaxFamily(OpContext& ctx) { HandleVecMinFamily(ctx); }

void Writer::HandleVecPackFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    const auto left = VectorValue(cursor.Take(), 0);
    auto right = VectorValue(cursor.Take(), 1);
    const auto bits = IntegerImmediate(cursor.Take()),
               unsigned_destination = IntegerImmediate(cursor.Take());
    if ((bits != 16 && bits != 32) || unsigned_destination > 1)
        Reject(CompileError::UnsupportedOperand);
    if (right == ctx.destination) {
        VectorMove(VectorTemporary(), right);
        right = VectorTemporary();
    }
    const auto low = Lanes(ctx.destination, bits / 2, 64), full = Lanes(ctx.destination, bits / 2);
    if (unsigned_destination) {
        __ sqxtun(low, Lanes(left, bits));
        __ sqxtun2(full, Lanes(right, bits));
    } else {
        __ sqxtn(low, Lanes(left, bits));
        __ sqxtn2(full, Lanes(right, bits));
    }
}

void Writer::HandleVecExtract64Family(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto input = VectorValue(cursor.Take(), 0);
    const auto lane = IntegerImmediate(cursor.Take());
    const bool half = operation.opcode == OpCode::VecExtract16;
    if (lane >= (half ? 8u : 2u)) Reject(CompileError::UnsupportedOperand);
    __ umov(half ? W(destination) : X(destination), Lanes(input, half ? 16 : 64), lane);
    Narrow(destination, destination, Width(operation.type));
}

void Writer::HandleVecMovMaskFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    if (operation.type != Type::U32 && operation.type != Type::U64)
        Reject(CompileError::UnsupportedType);
    const auto input = VectorValue(cursor.Take(), 0);
    const auto bits = IntegerImmediate(cursor.Take());
    LaneSize(bits);
    // Normalize arbitrary input lanes to one sign bit before
    // accumulating pairs. Each 64-bit half's mask ends in its
    // low byte; higher intermediate bits are not observed.
    __ ushr(Lanes(0, bits), Lanes(input, bits), bits - 1);  // USHR v0.
    for (unsigned lane = static_cast<unsigned>(bits) * 2, shift = static_cast<unsigned>(bits) - 1;
         lane <= 64;
         lane *= 2, shift *= 2)
        __ usra(Lanes(0, lane), Lanes(0, lane), shift);  // USRA v0,v0.
    __ umov(W(destination), Lanes(0, 8), 0);             // UMOV Wdestination,v0.b[0].
    __ umov(v::w17, Lanes(0, 8), 8);                     // UMOV W17,v0.b[8].
    __ orr(W(destination),
           W(destination),
           Operand(v::w17,
                   v::LSL,
                   64 / bits));  // ORR W; clears the complete upper result.
}

void Writer::HandleVecExtendFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    auto input = VectorValue(cursor.Take(), 0);
    const auto source_bits = IntegerImmediate(cursor.Take()),
               destination_bits = IntegerImmediate(cursor.Take());
    const auto sign = IntegerImmediate(cursor.Take());
    if ((source_bits != 8 && source_bits != 16 && source_bits != 32) ||
        (destination_bits != 16 && destination_bits != 32 && destination_bits != 64) ||
        source_bits >= destination_bits || sign > 1)
        Reject(CompileError::UnsupportedOperand);
    // SSHLL/USHLL #0 reads the low 64 bits before replacing the
    // complete destination. Repeating on that destination keeps
    // exactly the final output's low source lanes, including aliases.
    for (auto bits = source_bits; bits < destination_bits; bits *= 2) {
        if (sign)
            __ sshll(Lanes(destination, bits * 2), Lanes(input, bits, 64), 0);
        else
            __ ushll(Lanes(destination, bits * 2), Lanes(input, bits, 64), 0);
        input = destination;
    }
}

void Writer::HandleVecXorFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const bool logical = operation.opcode == OpCode::VecXor || operation.opcode == OpCode::VecAnd ||
                         operation.opcode == OpCode::VecOr || operation.opcode == OpCode::VecAndNot;
    const auto bits = logical ? 8u
                              : 8u << LaneSize(IntegerImmediate(cursor.Take()),
                                               operation.opcode != OpCode::VecMul);
    const auto vd = Lanes(destination, bits), vn = Lanes(left, bits), vm = Lanes(right, bits);
    switch (operation.opcode) {
        case OpCode::VecXor:
            __ eor(vd, vn, vm);
            break;
        case OpCode::VecAnd:
            __ and_(vd, vn, vm);
            break;
        case OpCode::VecOr:
            __ orr(vd, vn, vm);
            break;
        case OpCode::VecAndNot:
            __ bic(vd, vn, vm);
            break;
        case OpCode::VecAdd:
            __ add(vd, vn, vm);
            break;
        case OpCode::VecSub:
            __ sub(vd, vn, vm);
            break;
        case OpCode::VecMul:
            __ mul(vd, vn, vm);
            break;
        case OpCode::VecCmpEq:
            __ cmeq(vd, vn, vm);
            break;
        default:
            __ cmgt(vd, vn, vm);
            break;
    }
}

void Writer::HandleVecZipFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto size = LaneSize(IntegerImmediate(cursor.Take()));
    const auto high = IntegerImmediate(cursor.Take());
    if (high > 1) Reject(CompileError::UnsupportedOperand);
    const auto vd = Lanes(destination, 8u << size), vn = Lanes(left, 8u << size),
               vm = Lanes(right, 8u << size);
    if (operation.opcode == OpCode::VecZip) {
        if (high)
            __ zip2(vd, vn, vm);
        else
            __ zip1(vd, vn, vm);
    } else {
        if (high)
            __ uzp2(vd, vn, vm);
        else
            __ uzp1(vd, vn, vm);
    }
}

void Writer::HandleVecSignedMulWidenLowFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto vector = ctx.vector;
    if (!vector) Reject(CompileError::UnsupportedType);
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto size = LaneSize(IntegerImmediate(cursor.Take()), false);
    // Read the low contiguous half of both inputs, then replace all
    // 128 result bits. Either input may share the destination home.
    __ smull(Lanes(destination, 16u << size),
             Lanes(left, 8u << size, 64),
             Lanes(right, 8u << size, 64));
}

void Writer::HandleVecMulWidenFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto first_temp = VectorTemporary();
    const auto second_temp = VectorTemporary(true);
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto size = LaneSize(IntegerImmediate(cursor.Take()), false);
    const auto sign = IntegerImmediate(cursor.Take());
    if (sign > 1) Reject(CompileError::UnsupportedOperand);
    __ uzp1(Lanes(first_temp, 8u << size), Lanes(left, 8u << size), Lanes(left, 8u << size));
    __ uzp1(Lanes(second_temp, 8u << size), Lanes(right, 8u << size), Lanes(right, 8u << size));
    if (sign)
        __ smull(Lanes(destination, 16u << size),
                 Lanes(first_temp, 8u << size, 64),
                 Lanes(second_temp, 8u << size, 64));
    else
        __ umull(Lanes(destination, 16u << size),
                 Lanes(first_temp, 8u << size, 64),
                 Lanes(second_temp, 8u << size, 64));
}

void Writer::HandleVecMadd16Family(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto first_temp = VectorTemporary();
    const auto second_temp = VectorTemporary(true);
    const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    __ smull(Lanes(first_temp, 32), Lanes(left, 16, 64), Lanes(right, 16, 64));
    __ smull2(Lanes(second_temp, 32), Lanes(left, 16), Lanes(right, 16));
    __ addp(Lanes(destination, 32), Lanes(first_temp, 32), Lanes(second_temp, 32));
}

void Writer::HandleVecShuffle32TwoSrcFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto first_temp = VectorTemporary();
    const auto second_temp = VectorTemporary(true);
    auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    const auto control = IntegerImmediate(cursor.Take());
    if (control > 255) Reject(CompileError::UnsupportedOperand);
    // A coalesced publication can target one of the input pins.
    // Preserve that complete input before the first lane is written.
    if (left == destination) {
        VectorMove(first_temp, left);
        if (right == left) right = first_temp;
        left = first_temp;
    }
    if (right == destination) {
        VectorMove(second_temp, right);
        right = second_temp;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const auto selected = static_cast<u32>((control >> (i * 2)) & 3);
        const auto input = i < 2 ? left : right;
        __ ins(Lanes(destination, 32), i, Lanes(input, 32), selected);
    }
}

void Writer::HandleVecShuffle32Family(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto first_temp = VectorTemporary();
    auto input = VectorValue(cursor.Take(), 0);
    const auto control = IntegerImmediate(cursor.Take());
    if (control > 255) Reject(CompileError::UnsupportedOperand);
    const bool half = operation.opcode == OpCode::VecShuffle16;
    const auto high = half ? IntegerImmediate(cursor.Take()) : 0;
    if (high > 1) Reject(CompileError::UnsupportedOperand);
    const unsigned base = high ? 4 : 0, size = half ? 1 : 2;
    if (!half && ((control & 3) * 0x55 == control)) {
        __ dup(Lanes(destination, 32), Lanes(input, 32), control & 3);
    } else if (!half && (control == 0xa0 || control == 0xf5 || control == 0x50 || control == 0xfa ||
                         control == 0x88 || control == 0xdd)) {
        const auto vd = Lanes(destination, 32), vn = Lanes(input, 32);
        if (control == 0xa0)
            __ trn1(vd, vn, vn);
        else if (control == 0xf5)
            __ trn2(vd, vn, vn);
        else if (control == 0x50)
            __ zip1(vd, vn, vn);
        else if (control == 0xfa)
            __ zip2(vd, vn, vn);
        else if (control == 0x88)
            __ uzp1(vd, vn, vn);
        else
            __ uzp2(vd, vn, vn);
    } else if (!half && control == 0xe4) {
        VectorMove(destination, input);
    } else {
        if (input == destination) {
            VectorMove(first_temp, input);
            input = first_temp;
        }
        if (half) VectorMove(destination, input);
        for (unsigned i = 0; i < 4; ++i) {
            const auto selected = base + ((control >> (i * 2)) & 3);
            __ ins(Lanes(destination, 8u << size), base + i, Lanes(input, 8u << size), selected);
        }
    }
}

void Writer::HandleVecShiftLeftImmFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto input = VectorValue(cursor.Take(), 0);
    auto count = IntegerImmediate(cursor.Take());
    const auto bits = IntegerImmediate(cursor.Take());
    LaneSize(bits);
    const bool arithmetic = operation.opcode == OpCode::VecShiftRightArithmeticImm;
    if (!count) {
        VectorMove(destination, input);
        return;
    }
    if (count >= bits && !arithmetic) {
        __ eor(Lanes(destination, 8), Lanes(destination, 8), Lanes(destination, 8));
        return;
    }
    count = std::min(count, bits);
    const bool left = operation.opcode == OpCode::VecShiftLeftImm;
    if (left)
        __ shl(Lanes(destination, bits), Lanes(input, bits), count);
    else if (arithmetic)
        __ sshr(Lanes(destination, bits), Lanes(input, bits), count);
    else
        __ ushr(Lanes(destination, bits), Lanes(input, bits), count);
}

void Writer::HandleVecByteShiftFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
    auto count = IntegerImmediate(cursor.Take());
    if (operation.opcode == OpCode::VecByteShift) {
        const auto direction = IntegerImmediate(cursor.Take());
        if (!count || count >= 16 || direction > 1) Reject(CompileError::UnsupportedOperand);
        if (direction) {
            std::swap(left, right);
            count = 16 - count;
        }
    } else if (count >= 16)
        Reject(CompileError::UnsupportedOperand);
    __ ext(Lanes(destination, 8), Lanes(left, 8), Lanes(right, 8), count);
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
