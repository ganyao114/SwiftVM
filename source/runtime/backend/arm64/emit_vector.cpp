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
    const auto left_cell = cursor.Take(), right_cell = cursor.Take();
    if ((operation.opcode == OpCode::VecAnd || operation.opcode == OpCode::VecAndNot) &&
        operation.Metadata() == OperationMetadata{} && !ctx.home.spilled) {
        if (const auto c2 = NextNonNop(node.value + 1)) {
            const auto& op2 = source.Operations()[c2.value];
            const auto& home2 = allocation.homes[c2.value];
            const auto expected_op2 =
                    operation.opcode == OpCode::VecAnd ? OpCode::VecAndNot : OpCode::VecAnd;
            if (op2.opcode == expected_op2 && op2.block == operation.block &&
                op2.Metadata() == OperationMetadata{} && !home2.spilled) {
                if (const auto c3 = NextNonNop(c2.value + 1)) {
                    const auto& op3 = source.Operations()[c3.value];
                    const auto& home3 = allocation.homes[c3.value];
                    if (op3.opcode == OpCode::VecOr && op3.block == operation.block &&
                        op3.Metadata() == OperationMetadata{} && !home3.spilled &&
                        ctx.home.last_use == home3.definition_position &&
                        home2.last_use == home3.definition_position) {
                        const auto args2 = source.Operands(c2);
                        const auto args3 = source.Operands(c3);
                        auto it2 = args2.begin(), it3 = args3.begin();
                        if (it2 != args2.end() && it3 != args3.end()) {
                            const auto l2 = *it2++;
                            const auto l3 = *it3++;
                            if (it2 != args2.end() && it3 != args3.end()) {
                                const auto r2 = *it2++;
                                const auto r3 = *it3++;
                                const bool c2_uses_c1 =
                                        (l2.Kind() == OperandKind::Value &&
                                         l2.Payload() == node.value) ||
                                        (r2.Kind() == OperandKind::Value &&
                                         r2.Payload() == node.value);
                                const bool c3_pair =
                                        l3.Kind() == OperandKind::Value &&
                                        r3.Kind() == OperandKind::Value &&
                                        ((l3.Payload() == node.value && r3.Payload() == c2.value) ||
                                         (l3.Payload() == c2.value && r3.Payload() == node.value));
                                if (it2 == args2.end() && it3 == args3.end() && !c2_uses_c1 &&
                                    c3_pair) {
                                    const auto peel_val = [&](CellView c) -> u32 {
                                        if (c.Kind() != OperandKind::Value ||
                                            c.Payload() >= source.Operations().size())
                                            return UINT32_MAX;
                                        auto id = static_cast<u32>(c.Payload());
                                        for (unsigned d = 0; d < 4; ++d) {
                                            const auto& cur = source.Operations()[id];
                                            if (cur.opcode != OpCode::BitCast ||
                                                cur.type != Type::V128 ||
                                                cur.Metadata() != OperationMetadata{})
                                                break;
                                            const auto a = source.Operands(NodeId{id});
                                            if (a.begin() == a.end() ||
                                                a.front().Kind() != OperandKind::Value ||
                                                a.front().Payload() >= source.Operations().size())
                                                break;
                                            id = static_cast<u32>(a.front().Payload());
                                        }
                                        return id;
                                    };
                                    const bool c1_is_and = operation.opcode == OpCode::VecAnd;
                                    const CellView and_lhs = c1_is_and ? left_cell : l2;
                                    const CellView and_rhs = c1_is_and ? right_cell : r2;
                                    const CellView bic_val = c1_is_and ? l2 : left_cell;
                                    const CellView bic_mask = c1_is_and ? r2 : right_cell;
                                    const u32 mask_id = peel_val(bic_mask);
                                    CellView true_cell{};
                                    bool matched = false;
                                    if (mask_id != UINT32_MAX) {
                                        if (peel_val(and_rhs) == mask_id) {
                                            true_cell = and_lhs;
                                            matched = true;
                                        } else if (peel_val(and_lhs) == mask_id) {
                                            true_cell = and_rhs;
                                            matched = true;
                                        }
                                    }
                                    if (matched) {
                                        const unsigned m = VectorValue(bic_mask, 0);
                                        const unsigned t = VectorValue(true_cell, 1);
                                        const unsigned f =
                                                VectorValue(bic_val, VectorTemporary());
                                        const unsigned out = home3.reg;
                                        if (pending_denormal.op_count &&
                                            ((pending_denormal.protected_mask >> out) & 1u)) {
                                            if (!(HasUpcomingBatchOp(c3.value + 1) &&
                                                  PreserveBatchedRegister(out)))
                                                FlushDenormalBatch();
                                        }
                                        if (t == f) {
                                            VectorMove(out, t);
                                        } else if (out == m) {
                                            __ bsl(Lanes(out, 8), Lanes(t, 8), Lanes(f, 8));
                                        } else if (out == f) {
                                            __ bit(Lanes(out, 8), Lanes(t, 8), Lanes(m, 8));
                                        } else if (out == t) {
                                            __ bif(Lanes(out, 8), Lanes(f, 8), Lanes(m, 8));
                                        } else {
                                            VectorMove(out, m);
                                            __ bsl(Lanes(out, 8), Lanes(t, 8), Lanes(f, 8));
                                        }
                                        ElideConsumer(c3);
                                        ctx.saved = true;
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    const auto left = VectorValue(left_cell, 0), right = VectorValue(right_cell, 1);
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
    const auto left_cell = cursor.Take(), right_cell = cursor.Take();
    const bool right_unspilled = right_cell.Kind() == OperandKind::Value &&
                                 right_cell.Payload() < allocation.homes.size() &&
                                 !allocation.homes[right_cell.Payload()].spilled;
    const bool check_reuse = right_unspilled && output.words.size() == vec_mul_widen_words;
    const auto left = VectorValue(left_cell, 0), right = VectorValue(right_cell, 1);
    const auto size = LaneSize(IntegerImmediate(cursor.Take()), false);
    const auto sign = IntegerImmediate(cursor.Take());
    if (sign > 1) Reject(CompileError::UnsupportedOperand);
    __ uzp1(Lanes(first_temp, 8u << size), Lanes(left, 8u << size), Lanes(left, 8u << size));
    if (!(check_reuse && vec_mul_widen_right == right && vec_mul_widen_size == size))
        __ uzp1(Lanes(second_temp, 8u << size), Lanes(right, 8u << size), Lanes(right, 8u << size));
    if (sign)
        __ smull(Lanes(destination, 16u << size),
                 Lanes(first_temp, 8u << size, 64),
                 Lanes(second_temp, 8u << size, 64));
    else
        __ umull(Lanes(destination, 16u << size),
                 Lanes(first_temp, 8u << size, 64),
                 Lanes(second_temp, 8u << size, 64));
    if (right_unspilled && !ctx.home.spilled && destination != right) {
        vec_mul_widen_words = static_cast<u32>(output.words.size());
        vec_mul_widen_right = static_cast<std::uint8_t>(right);
        vec_mul_widen_size = static_cast<std::uint8_t>(size);
    } else {
        vec_mul_widen_words = UINT32_MAX;
    }
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
    } else if (!half && (control == 0x44 || control == 0xee)) {
        __ dup(Lanes(destination, 64), Lanes(input, 64), control == 0xee ? 1 : 0);
    } else if (!half && (control == 0x39 || control == 0x4e || control == 0x93)) {
        const unsigned imm = control == 0x39 ? 4 : control == 0x4e ? 8 : 12;
        __ ext(Lanes(destination, 8), Lanes(input, 8), Lanes(input, 8), imm);
    } else if (!half && control == 0xb1) {
        __ rev64(Lanes(destination, 32), Lanes(input, 32));
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
    if (operation.opcode == OpCode::VecShiftRightImm && count == 32 && bits == 64 &&
        !ctx.home.spilled) {
        if (const auto consumer = NextNonNop(node.value + 1)) {
            const auto& next = source.Operations()[consumer.value];
            const auto& next_home = allocation.homes[consumer.value];
            if (next.opcode == OpCode::VecMulWiden && next.block == operation.block &&
                next.Metadata() == OperationMetadata{} && !next_home.spilled &&
                ctx.home.last_use == next_home.definition_position) {
                const auto inputs = source.Operands(consumer);
                auto at = inputs.begin();
                if (at != inputs.end()) {
                    const auto lhs_cell = *at++;
                    if (at != inputs.end()) {
                        const auto rhs_cell = *at++;
                        if (at != inputs.end()) {
                            const auto size = LaneSize(IntegerImmediate(*at++), false);
                            if (at != inputs.end() && size == 2) {
                                const auto sign = IntegerImmediate(*at++);
                                const bool lhs_is_node = lhs_cell.Kind() == OperandKind::Value &&
                                                         lhs_cell.Payload() == node.value;
                                const bool rhs_is_node = rhs_cell.Kind() == OperandKind::Value &&
                                                         rhs_cell.Payload() == node.value;
                                if (at == inputs.end() && sign <= 1 &&
                                    (lhs_is_node || rhs_is_node)) {
                                    if (pending_denormal.op_count &&
                                        ((pending_denormal.protected_mask >> next_home.reg) & 1u)) {
                                        if (!(HasUpcomingBatchOp(consumer.value + 1) &&
                                              PreserveBatchedRegister(next_home.reg)))
                                            FlushDenormalBatch();
                                    }
                                    const auto first_temp = VectorTemporary();
                                    const auto second_temp = VectorTemporary(true);
                                    if (lhs_is_node && rhs_is_node) {
                                        __ uzp2(Lanes(first_temp, 32),
                                                Lanes(input, 32),
                                                Lanes(input, 32));
                                        if (sign)
                                            __ smull(Lanes(next_home.reg, 64),
                                                     Lanes(first_temp, 32, 64),
                                                     Lanes(first_temp, 32, 64));
                                        else
                                            __ umull(Lanes(next_home.reg, 64),
                                                     Lanes(first_temp, 32, 64),
                                                     Lanes(first_temp, 32, 64));
                                        vec_mul_widen_words = UINT32_MAX;
                                    } else {
                                        const auto other_cell = lhs_is_node ? rhs_cell : lhs_cell;
                                        const bool right_unspilled =
                                                other_cell.Kind() == OperandKind::Value &&
                                                other_cell.Payload() < allocation.homes.size() &&
                                                !allocation.homes[other_cell.Payload()].spilled;
                                        const bool check_reuse =
                                                right_unspilled &&
                                                output.words.size() == vec_mul_widen_words;
                                        const auto right = VectorValue(other_cell, 1);
                                        __ uzp2(Lanes(first_temp, 32),
                                                Lanes(input, 32),
                                                Lanes(input, 32));
                                        if (!(check_reuse && vec_mul_widen_right == right &&
                                              vec_mul_widen_size == 2)) {
                                            __ uzp1(Lanes(second_temp, 32),
                                                    Lanes(right, 32),
                                                    Lanes(right, 32));
                                        }
                                        if (sign)
                                            __ smull(Lanes(next_home.reg, 64),
                                                     Lanes(first_temp, 32, 64),
                                                     Lanes(second_temp, 32, 64));
                                        else
                                            __ umull(Lanes(next_home.reg, 64),
                                                     Lanes(first_temp, 32, 64),
                                                     Lanes(second_temp, 32, 64));
                                        if (right_unspilled && next_home.reg != right) {
                                            vec_mul_widen_words =
                                                    static_cast<u32>(output.words.size());
                                            vec_mul_widen_right = static_cast<std::uint8_t>(right);
                                            vec_mul_widen_size = 2;
                                        } else {
                                            vec_mul_widen_words = UINT32_MAX;
                                        }
                                    }
                                    ElideConsumer(consumer);
                                    ctx.saved = true;
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
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
