#include "emit.h"

namespace swift::runtime::backend::arm64::detail {

void Writer::LoadMxcsrFlushControl() {
    const auto offset = options.mxcsr_flush_offset;
    if (!(offset & 3) && offset / 4 <= 4095) Memory(true, 32, 0, 19, offset / 4);
    else {
        Immediate(17, offset); Put(0x8b000000u | (17u << 16) | (19u << 5) | 17u);
        Memory(true, 32, 0, 17);
    }
}

void Writer::FlushScalarDenormal(unsigned value, unsigned control_bit, unsigned lane) {
    // x0 holds the actual MXCSR controls. Neither branch changes NZCV.
    const auto disabled = static_cast<u32>(output.words.size());
    Put(0x36000000u | (control_bit << 19)); // TBZ w0,control_bit,done.
    Put(0x4e083c00u | (lane << 20) | (value << 5) | 16u); // UMOV x16,Vvalue.d[lane].
    Extract(17, 16, 52, 11);
    const auto normal = static_cast<u32>(output.words.size());
    Put(0xb5000011u); // CBNZ x17,done: zero exponent includes existing zeros.
    u32 fields{}; LogicalImmediate(u64{1} << 63, 64, fields);
    Put(0x92000000u | fields | (16u << 5) | 16u); // Keep only the IEEE sign bit.
    Put(0x4e081c00u | (lane << 20) | (16u << 5) | value); // INS preserves the other D lane.
    const auto done = static_cast<u32>(output.words.size());
    output.words[disabled] |= (done - disabled) << 5;
    output.words[normal] |= (done - normal) << 5;
}

void Writer::TruncateDoubleToInteger(unsigned destination, unsigned raw, unsigned bits) {
    Put(0x9e670000u | (raw << 5)); // FMOV d0,Xraw: capture before any output overwrite.
    Extract(17, raw, 52, 11);
    Put(0xd1000000u | ((1023u + bits - 1) << 10) | (17u << 5) | 17u);
    Put((bits == 32 ? 0x1e780000u : 0x9e780000u) | destination); // FCVTZS Wd/Xd,d0.
    // exp >= bias+width-1 covers NaN/Inf/overflow. The valid negative
    // boundary has the same minInt pattern. No guest NZCV is modified.
    const auto valid = static_cast<u32>(output.words.size());
    Put(0xb7f80011u); // TBNZ x17,#63,done.
    Immediate(destination, u64{1} << (bits - 1));
    output.words[valid] |= (static_cast<u32>(output.words.size()) - valid) << 5;
}

unsigned Writer::VectorValue(CellView cell, unsigned scratch) {
    if (cell.Kind() != OperandKind::Value || cell.Payload() >= allocation.homes.size() ||
        source.Operations()[cell.Payload()].type != Type::V128)
        Reject(CompileError::UnsupportedType);
    const auto& home = allocation.homes[cell.Payload()];
    if (!home.spilled) return home.reg;
    VectorMemory(true, scratch, 31, home.stack_offset / 16); return scratch;
}

int Writer::VectorPinnedSlot(u32 offset, unsigned bytes) const {
    if (!VectorPinned() || bytes != 16 || (offset & 15) || offset < options.pinned_vector_offset ||
            u64{offset} + bytes > u64{options.pinned_vector_offset} + 256 ||
            ((offset - options.pinned_vector_offset) & 15)) return -1;
    return static_cast<int>(abi::PinnedVectorRegister((offset - options.pinned_vector_offset) / 16));
}

void Writer::VectorAlias(bool reload, u32 offset, unsigned bytes) {
    if (!VectorPinned() || !bytes) return;
    const u64 begin = options.pinned_vector_offset, end = begin + 256;
    if (offset >= end || begin >= u64{offset} + bytes) return;
    const auto first = (std::max(u64{offset}, begin) - begin) / 16;
    const auto last = (std::min(u64{offset} + bytes, end) - 1 - begin) / 16;
    for (auto slot = first; slot <= last; ++slot) {
        const auto field = begin + slot * 16;
        const auto reg = abi::PinnedVectorRegister(static_cast<unsigned>(slot));
        if (!(field & 15) && field / 16 <= 4095)
            VectorMemory(reload, reg, 19, static_cast<u32>(field / 16));
        else {
            Immediate(17, field); Put(0x8b000000u | (17u << 16) | (19u << 5) | 17u);
            VectorMemory(reload, reg, 17);
        }
    }
}

void Writer::VectorUniform(bool load, CellView uniform, unsigned value) {
    if (uniform.Kind() != OperandKind::Uniform || uniform.Payload() > UINT32_MAX || uniform.TypeView() != Type::V128)
        Reject(CompileError::UnsupportedOperand);
    const auto offset = static_cast<u32>(uniform.Payload());
    (void)PinnedSlot(offset, 16); // Cross-slot vector accesses are outside this explicit ABI.
    if (const auto pinned = VectorPinnedSlot(offset, 16); pinned >= 0) {
        if (load) VectorMove(value, static_cast<unsigned>(pinned));
        else VectorMove(static_cast<unsigned>(pinned), value);
        return;
    }
    VectorAlias(false, offset, 16);
    if (!(offset & 15) && offset / 16 <= 4095) VectorMemory(load, value, 19, offset / 16);
    else {
        Immediate(17, offset); Put(0x8b000000u | (17u << 16) | (19u << 5) | 17u);
        VectorMemory(load, value, 17);
    }
    if (!load) VectorAlias(true, offset, 16);
}

bool Writer::EmitVector(const Operation& operation, Cursor& cursor, unsigned destination) {
    const bool vector = operation.type == Type::V128;
    const auto first_temp = VectorTemporary(), second_temp = VectorTemporary(true);
    switch (operation.opcode) {
        case OpCode::LoadUniform: {
            if (!vector) return false;
            const auto uniform = cursor.Take();
            if (OverlapsShadow(uniform)) Shadow(false);
            VectorUniform(true, uniform, destination); break;
        }
        case OpCode::LoadMemory: case OpCode::LoadMemoryTSO: {
            if (!vector) return false;
            if (!options.private_uniforms) Shadow(false);
            VectorMemory(true, destination, Expression(cursor, 16, 17));
            if (operation.opcode == OpCode::LoadMemoryTSO) Put(0xd50339bf); // DMB ISHLD.
            break;
        }
        case OpCode::BitCast:
            if (!vector) return false;
            VectorMove(destination, VectorValue(cursor.Take(), 0)); break;
        case OpCode::VecSharedZero:
            Put(0x6e201c00u | (destination << 16) | (destination << 5) | destination); break;
        case OpCode::VecLoadConst: {
            const auto low = IntegerImmediate(cursor.Take()), high = IntegerImmediate(cursor.Take());
            Immediate(16, low); Put(0x9e670000u | (16u << 5) | destination);
            Immediate(17, high); Put(0x4e181c00u | (17u << 5) | destination); break;
        }
        case OpCode::VecDup64: {
            const auto input = Read(cursor.Take(), 16);
            Put(0x4e080c00u | (input << 5) | destination); break;
        }
        case OpCode::VecFCvtIntToFloat: {
            const auto input = cursor.Take();
            const auto source_bits = IntegerImmediate(cursor.Take()), float_bits = IntegerImmediate(cursor.Take());
            if ((source_bits != 32 && source_bits != 64) || float_bits != 64 ||
                    Width(View(input)) != source_bits || operation.type != Type::U64)
                Reject(CompileError::UnsupportedOperand);
            const auto value = RawScalar(input, 16);
            Put((source_bits == 32 ? 0x1e620000u : 0x9e620000u) | (value << 5)); // SCVTF d0,Wn/Xn.
            Put(0x9e660000u | destination); // FMOV destination,d0: raw IEEE bits.
            break;
        }
        case OpCode::VecFCvtFloatToInt: {
            const auto input = cursor.Take();
            const auto float_bits = IntegerImmediate(cursor.Take()), integer_bits = IntegerImmediate(cursor.Take());
            const auto truncate = IntegerImmediate(cursor.Take());
            if (float_bits != 64 || (integer_bits != 32 && integer_bits != 64) || truncate != 1 ||
                    Width(View(input)) != 64 || operation.type != (integer_bits == 32 ? Type::U32 : Type::U64))
                Reject(CompileError::UnsupportedOperand);
            const auto value = RawScalar(input, 16);
            TruncateDoubleToInteger(destination, value, static_cast<unsigned>(integer_bits));
            break;
        }
        case OpCode::VecFAddScalar64: case OpCode::VecFSubScalar64:
        case OpCode::VecFMulScalar64: case OpCode::VecFDivScalar64: {
            if (!vector) Reject(CompileError::UnsupportedType);
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            const auto opcode = operation.opcode == OpCode::VecFAddScalar64 ? 0x1e602800u :
                    operation.opcode == OpCode::VecFSubScalar64 ? 0x1e603800u :
                    operation.opcode == OpCode::VecFMulScalar64 ? 0x1e600800u : 0x1e601800u;
            if (options.mxcsr_flush_offset != UINT32_MAX) {
                VectorMove(first_temp, left); VectorMove(second_temp, right);
                LoadMxcsrFlushControl();
                FlushScalarDenormal(first_temp, 6); FlushScalarDenormal(second_temp, 6);
                Put(opcode | (second_temp << 16) | (first_temp << 5) | first_temp);
                FlushScalarDenormal(first_temp, 15);
            } else Put(opcode | (right << 16) | (left << 5) | first_temp);
            // Scalar FP arithmetic may clear its upper lane. Preserve the complete
            // left operand explicitly rather than depend on FEAT_AFP.NEP.
            VectorMove(destination, left);
            Put(0x6e080400u | (first_temp << 5) | destination); // INS destination.d[0],temporary.d[0].
            break;
        }
        case OpCode::VecFCmp: {
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            if (IntegerImmediate(cursor.Take()) != 64) Reject(CompileError::UnsupportedOperand);
            unsigned a = left, b = right;
            if (options.mxcsr_flush_offset != UINT32_MAX) {
                VectorMove(first_temp, left); VectorMove(second_temp, right);
                LoadMxcsrFlushControl();
                FlushScalarDenormal(first_temp, 6); FlushScalarDenormal(second_temp, 6);
                a = first_temp; b = second_temp;
            }
            Put(0xd53b421eu); // Preserve the actual guest/packed-mode host NZCV.
            Put(0x1e602000u | (b << 16) | (a << 5)); // FCMP Da,Db.
            Put(0x1a9f07e0u | (10u << 12) | destination); // CSET Wdestination,LT: less or unordered.
            Put(0x1a9f07e0u | (7u << 12)); // CSET W0,VS: unordered (destination may be spill scratch16).
            Put(0x1a9f07e0u | (1u << 12) | 17u); // CSET W17,EQ.
            Put(0x2a000000u | (17u << 5) | 17u); // Equal or unordered.
            Put(0x2a000000u | (1u << 10) | (destination << 5) | destination);
            Put(0x2a000000u | (17u << 16) | (2u << 10) | (destination << 5) | destination);
            Put(0xd51b421eu); // MSR NZCV,x30: publishing flags is a separate op.
            break;
        }
        case OpCode::VecFCvtPacked: {
            const auto input = VectorValue(cursor.Take(), 0);
            const auto kind = IntegerImmediate(cursor.Take());
            if (kind == 1) { // Signed low 2x32 -> exact 2x64 IEEE doubles.
                Put(0x0f20a400u | (input << 5) | destination); // SXTL Vd.2d,Vn.2s.
                Put(0x4e61d800u | (destination << 5) | destination); // SCVTF Vd.2d,Vd.2d.
            } else if (kind == 5) { // Truncate 2x64 IEEE doubles -> signed low 2x32.
                // A spilled input can be v0; scalar truncation uses d0.
                // Keep both raw lanes before converting either one.
                VectorMove(first_temp, input);
                Put(0x6e201c00u | (destination << 16) | (destination << 5) | destination);
                for (unsigned lane = 0; lane < 2; ++lane) {
                    Put(0x4e083c00u | (lane << 20) | (first_temp << 5) | 16u);
                    TruncateDoubleToInteger(0, 16, 32);
                    Put(0x4e041c00u | (lane << 19) | destination); // INS Vd.s[lane],w0; high64 stays zero.
                }
            } else Reject(CompileError::UnsupportedOperand);
            break;
        }
        case OpCode::VecFCmpMask: {
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            const auto bits = IntegerImmediate(cursor.Take()), relations = IntegerImmediate(cursor.Take());
            const auto scalar = IntegerImmediate(cursor.Take());
            if (bits != 64 || relations > 15 || scalar != 0) Reject(CompileError::UnsupportedOperand);
            const bool invert = relations & 8;
            const auto ordered = static_cast<unsigned>(invert ? ~relations & 7 : relations);
            if (!ordered) {
                // Integer self-comparison makes all ones without FP exceptions.
                Put((invert ? 0x6ee08c00u : 0x6e201c00u) |
                        (destination << 16) | (destination << 5) | destination);
                break;
            }
            unsigned a = left, b = right, temporary = first_temp;
            if (options.mxcsr_flush_offset != UINT32_MAX) {
                VectorMove(first_temp, left); VectorMove(second_temp, right);
                LoadMxcsrFlushControl();
                for (unsigned lane = 0; lane < 2; ++lane) {
                    FlushScalarDenormal(first_temp, 6, lane); FlushScalarDenormal(second_temp, 6, lane);
                }
                a = first_temp; b = second_temp; temporary = 0;
            }
            // When UN is selected, invert the complementary ordered set.
            // Each IEEE lane has exactly one LT/EQ/GT/UN outcome.
            if (ordered == 5 || ordered == 7) {
                Put((ordered == 5 ? 0x6ee0e400u : 0x6e60e400u) |
                        (b << 16) | (a << 5) | temporary);
                Put(0x6ee0e400u | (a << 16) | (b << 5) | destination);
                Put(0x4ea01c00u | (destination << 16) | (temporary << 5) | destination);
            } else {
                const auto opcode = ordered == 2 ? 0x4e60e400u :
                        (ordered == 3 || ordered == 6) ? 0x6e60e400u : 0x6ee0e400u;
                if (ordered == 1 || ordered == 3) std::swap(a, b);
                Put(opcode | (b << 16) | (a << 5) | destination);
            }
            if (invert) Put(0x6e205800u | (destination << 5) | destination); // NOT Vd.16b,Vd.16b.
            break;
        }
        case OpCode::VecExtract64: case OpCode::VecExtract16: {
            const auto input = VectorValue(cursor.Take(), 0);
            const auto lane = IntegerImmediate(cursor.Take());
            const bool half = operation.opcode == OpCode::VecExtract16;
            if (lane >= (half ? 8u : 2u)) Reject(CompileError::UnsupportedOperand);
            Put((half ? 0x0e023c00u : 0x4e083c00u) |
                    (static_cast<u32>(lane) << (half ? 18 : 20)) | (input << 5) | destination);
            Narrow(destination, destination, Width(operation.type)); break;
        }
        case OpCode::VecMovMask: {
            if (operation.type != Type::U32 && operation.type != Type::U64)
                Reject(CompileError::UnsupportedType);
            const auto input = VectorValue(cursor.Take(), 0);
            const auto bits = IntegerImmediate(cursor.Take());
            LaneSize(bits);
            // Normalize arbitrary input lanes to one sign bit before
            // accumulating pairs. Each 64-bit half's mask ends in its
            // low byte; higher intermediate bits are not observed.
            Put(0x6f000400u | (static_cast<u32>(bits + 1) << 16) | (input << 5)); // USHR v0.
            for (unsigned lane = static_cast<unsigned>(bits) * 2, shift = static_cast<unsigned>(bits) - 1;
                 lane <= 64; lane *= 2, shift *= 2)
                Put(0x6f001400u | ((2 * lane - shift) << 16)); // USRA v0,v0.
            Put(0x0e013c00u | destination); // UMOV Wdestination,v0.b[0].
            Put(0x0e113c00u | 17u);         // UMOV W17,v0.b[8].
            Put(0x2a000000u | (17u << 16) | (static_cast<u32>(64 / bits) << 10) |
                    (destination << 5) | destination); // ORR W; clears the complete upper result.
            break;
        }
        case OpCode::VecExtend: {
            auto input = VectorValue(cursor.Take(), 0);
            const auto source_bits = IntegerImmediate(cursor.Take()), destination_bits = IntegerImmediate(cursor.Take());
            const auto sign = IntegerImmediate(cursor.Take());
            if ((source_bits != 8 && source_bits != 16 && source_bits != 32) ||
                (destination_bits != 16 && destination_bits != 32 && destination_bits != 64) ||
                source_bits >= destination_bits || sign > 1)
                Reject(CompileError::UnsupportedOperand);
            // SSHLL/USHLL #0 reads the low 64 bits before replacing the
            // complete destination. Repeating on that destination keeps
            // exactly the final output's low source lanes, including aliases.
            for (auto bits = source_bits; bits < destination_bits; bits *= 2) {
                Put((sign ? 0x0f00a400u : 0x2f00a400u) | (static_cast<u32>(bits) << 16) |
                        (input << 5) | destination);
                input = destination;
            }
            break;
        }
        case OpCode::VecXor: case OpCode::VecAnd: case OpCode::VecOr: case OpCode::VecAndNot:
        case OpCode::VecAdd: case OpCode::VecSub: case OpCode::VecMul:
        case OpCode::VecCmpEq: case OpCode::VecCmpGt: {
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            auto opcode = operation.opcode == OpCode::VecXor ? 0x6e201c00u :
                    operation.opcode == OpCode::VecAnd ? 0x4e201c00u :
                    operation.opcode == OpCode::VecOr ? 0x4ea01c00u :
                    operation.opcode == OpCode::VecAndNot ? 0x4e601c00u :
                    operation.opcode == OpCode::VecAdd ? 0x4e208400u :
                    operation.opcode == OpCode::VecSub ? 0x6e208400u :
                    operation.opcode == OpCode::VecMul ? 0x4e209c00u :
                    operation.opcode == OpCode::VecCmpEq ? 0x6e208c00u : 0x4e203400u;
            if (operation.opcode != OpCode::VecXor && operation.opcode != OpCode::VecAnd &&
                operation.opcode != OpCode::VecOr && operation.opcode != OpCode::VecAndNot)
                opcode |= LaneSize(IntegerImmediate(cursor.Take()), operation.opcode != OpCode::VecMul) << 22;
            Put(opcode | (right << 16) | (left << 5) | destination); break;
        }
        case OpCode::VecZip: case OpCode::VecUnzip: {
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            const auto size = LaneSize(IntegerImmediate(cursor.Take()));
            const auto high = IntegerImmediate(cursor.Take());
            if (high > 1) Reject(CompileError::UnsupportedOperand);
            const auto opcode = operation.opcode == OpCode::VecZip ? (high ? 0x4e007800u : 0x4e003800u)
                    : (high ? 0x4e005800u : 0x4e001800u);
            Put(opcode | (size << 22) | (right << 16) | (left << 5) | destination); break;
        }
        case OpCode::VecSignedMulWidenLow: {
            if (!vector) Reject(CompileError::UnsupportedType);
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            const auto size = LaneSize(IntegerImmediate(cursor.Take()), false);
            // Read the low contiguous half of both inputs, then replace all
            // 128 result bits. Either input may share the destination home.
            Put(0x0e20c000u | (size << 22) | (right << 16) | (left << 5) | destination);
            break;
        }
        case OpCode::VecMulWiden: {
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            const auto size = LaneSize(IntegerImmediate(cursor.Take()), false);
            const auto sign = IntegerImmediate(cursor.Take());
            if (sign > 1) Reject(CompileError::UnsupportedOperand);
            Put(0x4e001800u | (size << 22) | (left << 16) | (left << 5) | first_temp);
            Put(0x4e001800u | (size << 22) | (right << 16) | (right << 5) | second_temp);
            Put((sign ? 0x0e20c000u : 0x2e20c000u) | (size << 22) |
                    (second_temp << 16) | (first_temp << 5) | destination);
            break;
        }
        case OpCode::VecMadd16: {
            const auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            Put(0x0e60c000u | (right << 16) | (left << 5) | first_temp);
            Put(0x4e60c000u | (right << 16) | (left << 5) | second_temp);
            Put(0x4ea0bc00u | (second_temp << 16) | (first_temp << 5) | destination); break;
        }
        case OpCode::VecShuffle32TwoSrc: {
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
            if (right == destination) { VectorMove(second_temp, right); right = second_temp; }
            for (unsigned i = 0; i < 4; ++i) {
                const auto selected = static_cast<u32>((control >> (i * 2)) & 3);
                const auto input = i < 2 ? left : right;
                const auto imm5 = 4u | (i << 3);
                Put(0x6e000400u | (imm5 << 16) | (selected << 13) | (input << 5) | destination);
            }
            break;
        }
        case OpCode::VecShuffle32: case OpCode::VecShuffle16: {
            auto input = VectorValue(cursor.Take(), 0);
            const auto control = IntegerImmediate(cursor.Take());
            if (control > 255) Reject(CompileError::UnsupportedOperand);
            const bool half = operation.opcode == OpCode::VecShuffle16;
            const auto high = half ? IntegerImmediate(cursor.Take()) : 0;
            if (high > 1) Reject(CompileError::UnsupportedOperand);
            const unsigned base = high ? 4 : 0, size = half ? 1 : 2;
            if (!half && ((control & 3) * 0x55 == control)) {
                Put(0x4e000400u | ((4u | (static_cast<u32>(control & 3) << 3)) << 16) | (input << 5) | destination);
            } else if (!half && (control == 0xa0 || control == 0xf5 || control == 0x50 || control == 0xfa ||
                                control == 0x88 || control == 0xdd)) {
                const auto opcode = control == 0xa0 ? 0x4e802800u : control == 0xf5 ? 0x4e806800u :
                        control == 0x50 ? 0x4e803800u : control == 0xfa ? 0x4e807800u :
                        control == 0x88 ? 0x4e801800u : 0x4e805800u;
                Put(opcode | (input << 16) | (input << 5) | destination);
            } else if (!half && control == 0xe4) {
                VectorMove(destination, input);
            } else {
                if (input == destination) { VectorMove(first_temp, input); input = first_temp; }
                if (half) VectorMove(destination, input);
                for (unsigned i = 0; i < 4; ++i) {
                    const auto selected = base + ((control >> (i * 2)) & 3);
                    const auto imm5 = (1u << size) | ((base + i) << (size + 1));
                    Put(0x6e000400u | (imm5 << 16) | (static_cast<u32>(selected) << (size + 11)) | (input << 5) | destination);
                }
            }
            break;
        }
        case OpCode::VecShiftLeftImm: case OpCode::VecShiftRightImm: case OpCode::VecShiftRightArithmeticImm: {
            const auto input = VectorValue(cursor.Take(), 0);
            auto count = IntegerImmediate(cursor.Take()); const auto bits = IntegerImmediate(cursor.Take());
            LaneSize(bits);
            const bool arithmetic = operation.opcode == OpCode::VecShiftRightArithmeticImm;
            if (!count) { VectorMove(destination, input); break; }
            if (count >= bits && !arithmetic) {
                Put(0x6e201c00u | (destination << 16) | (destination << 5) | destination); break;
            }
            count = std::min(count, bits);
            const bool left = operation.opcode == OpCode::VecShiftLeftImm;
            Put((left ? 0x4f005400u : arithmetic ? 0x4f000400u : 0x6f000400u) |
                    (static_cast<u32>(left ? bits + count : 2 * bits - count) << 16) | (input << 5) | destination);
            break;
        }
        case OpCode::VecByteShift: case OpCode::VecExtractBytes: {
            auto left = VectorValue(cursor.Take(), 0), right = VectorValue(cursor.Take(), 1);
            auto count = IntegerImmediate(cursor.Take());
            if (operation.opcode == OpCode::VecByteShift) {
                const auto direction = IntegerImmediate(cursor.Take());
                if (!count || count >= 16 || direction > 1) Reject(CompileError::UnsupportedOperand);
                if (direction) { std::swap(left, right); count = 16 - count; }
            } else if (count >= 16) Reject(CompileError::UnsupportedOperand);
            Put(0x6e000000u | (right << 16) | (static_cast<u32>(count) << 11) | (left << 5) | destination); break;
        }
        default: return false;
    }
    return true;
}

} // namespace swift::runtime::backend::arm64::detail
