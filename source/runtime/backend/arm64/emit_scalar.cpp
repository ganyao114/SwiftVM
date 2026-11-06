#include "emit.h"

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned, bool) noexcept;
std::uintptr_t CpuidHelper() noexcept;
std::uintptr_t X87Helper() noexcept;
namespace detail {
void Writer::HandleReadSystemFamily(OpContext& ctx) {
    const auto kind = IntegerImmediate(ctx.cursor.Take()),
               callback = IntegerImmediate(ctx.cursor.Take());
    if (!callback) Reject(CompileError::UnsupportedOperand);
    Shadow(false);
    PublishPinned();
    HelperFPEnvironment(false);
    if (kind == 1) masm.exact.dmb(v::InnerShareable, v::BarrierAll);
    Immediate(0, kind);
    Immediate(16, 0);
    Immediate(17, callback);
    Immediate(30, CpuidHelper());
    masm.exact.blr(v::x30);
    HelperFPEnvironment(true);
    if (kind == 1) masm.exact.isb();
    if (ctx.operation.opcode == OpCode::ReadSystemPair) {
        if (ctx.operation.type != Type::V128) Reject(CompileError::UnsupportedType);
        masm.exact.fmov(D(ctx.destination), v::x0);
        masm.exact.ins(Lanes(ctx.destination, 64), 1, v::x16);
    } else
        Move(ctx.destination, 0);
    nzcv_matches = 0;
    RestoreHelperNZCV();
}
bool Writer::PartialByteStore(const Operation& operation, const Home& home) const {
    if (operation.type != Type::U8 || home.spilled || home.reg == 1 ||
        node.value + 1 >= source.Operations().size())
        return false;
    const NodeId consumer{node.value + 1};
    const auto& next = source.Operations()[consumer.value];
    if (next.block != operation.block || next.opcode != OpCode::StoreUniform ||
        next.Metadata() != OperationMetadata{} ||
        home.last_use != allocation.homes[consumer.value].definition_position)
        return false;
    const auto fields = source.Operands(consumer);
    auto at = fields.begin();
    if (at == fields.end()) return false;
    const auto field = *at++;
    if (at == fields.end()) return false;
    const auto value = *at++;
    return at == fields.end() && field.Kind() == OperandKind::Uniform &&
           field.TypeView() == Type::U8 && field.Payload() <= UINT32_MAX &&
           value.Kind() == OperandKind::Value && value.Payload() == node.value &&
           View(value) == Type::U8 && PinnedSlot(static_cast<u32>(field.Payload()), 1) >= 0;
}

void Writer::HandleZeroFamily(OpContext& ctx) {
    const auto destination = ctx.destination;
    do {
        Immediate(destination, 0);
        break;
    } while (false);
}

void Writer::HandleLoadImmFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = cursor.Take();
            if (input.Kind() != OperandKind::Immediate) Reject(CompileError::UnsupportedOperand);
            Width(input.TypeView());
            if (!home.terminal_only)
                Immediate(
                        destination,
                        width == 64 ? input.Payload() : input.Payload() & ((u64{1} << width) - 1));
            break;
        }
    } while (false);
}

void Writer::HandleGetOperandFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto expression = *cursor.at;
            const bool word = width == 32 && expression.Kind() == OperandKind::Expression &&
                              expression.ShiftAmount() < 32;
            const auto value = Expression(cursor, destination, 17, word ? 32 : 64);
            if (word)
                Move(destination, value);
            else
                Narrow(destination, value, width);
            break;
        }
    } while (false);
}

void Writer::HandleBitCastFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto vector = ctx.vector;
    do {
        if (vector) {
            VectorMove(destination, VectorValue(cursor.Take(), 0));
            return;
        }
        {
            const auto value = cursor.Take();
            if (value.Kind() != OperandKind::Value) Reject(CompileError::UnsupportedOperand);
            Width(View(value));  // Raw transport still has a supported scalar view.
            Move(destination, RawValue(static_cast<u32>(value.Payload()), 16));
            break;
        }
    } while (false);
}

void Writer::HandleZeroExtend32Family(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = cursor.Take();
            if (node == elided_consumer) {
                elided_consumer = {};
                break;
            }
            const auto limit = operation.opcode == OpCode::ZeroExtend32 ||
                                               operation.opcode == OpCode::ZeroExtend32To64
                                       ? 32u
                                       : 64u;
            const bool sign = operation.opcode == OpCode::SignExtend;
            if (width == 32) {
                const auto source_bits = std::min(Width(View(input)), limit);
                const auto value = Read(input, destination, limit, false, 32);
                if (sign && source_bits < 32)
                    masm.exact.sbfx(W(destination), W(value), 0, source_bits);  // SXTB/SXTH W.
                else
                    Narrow(destination, value, 32);
            } else {
                const auto value = Read(input, destination, limit, sign);
                Narrow(destination, value, width);
            }
            break;
        }
    } while (false);
}

void Writer::HandleAddFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    const auto& metadata = ctx.metadata;
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    const auto flag_mask = ctx.flag_mask;
    const auto legacy_mask = ctx.legacy_mask;
    const auto test_prefix = ctx.test_prefix;
    auto& saved = ctx.saved;
    const auto save_result = [&] { SaveResult(ctx); };
    do {
        {
            if (node == elided_consumer) {
                cursor.Take();
                cursor.Take();
                elided_consumer = {};
                break;
            }
            if (test_prefix && operation.opcode == OpCode::And && DeferTest(operation, cursor)) {
                cursor.Done();
                return;
            }
            const auto input = cursor.Take();
            const auto operand = *cursor.at;
            const auto native_width = width == 32 ? 32u : 64u;
            if (RawAF() && !legacy_mask && operation.opcode == OpCode::And &&
                operation.type == Type::U64 && flag_mask == all_flags &&
                metadata.profile == FlagProfile::LogicalZeroCVA &&
                home.last_use == home.definition_position && !home.spilled &&
                input.Kind() == OperandKind::Value && operand.Kind() == OperandKind::Value &&
                input.Payload() == operand.Payload() && View(input) == Type::U64 &&
                View(operand) == Type::U64) {
                cursor.Take();
                const auto value = Read(input, 16);
                nzcv_matches = 0;
                PrepareNZCV(all_flags);
                masm.exact.subs(v::x1,
                                X(value),
                                Operand(0));  // SUBS x1,value,#0 also supplies inverted guest C=1.
                LogicalFlags(1, 64, all_flags, true, /*nzcv_ready=*/true, /*invert_carry=*/false);
                saved = true;
                break;
            }
            if (RawAF() && !legacy_mask && operation.opcode == OpCode::And &&
                (width == 8 || width == 16) && flag_mask == all_flags &&
                home.last_use == home.definition_position && !home.spilled &&
                input.Kind() == OperandKind::Value && operand.Kind() == OperandKind::Value &&
                input.Payload() == operand.Payload() && input.TypeView() == operand.TypeView()) {
                cursor.Take();
                const auto available = std::min(Width(View(input)),
                                                allocation.homes[input.Payload()].CarrierBits());
                const auto value = available >= width
                                           ? RawValue(static_cast<u32>(input.Payload()), 16, true)
                                           : Read(input, 16, width);
                // There is no numeric reader: x1 is only the raw PF carrier.
                // Its sign-extended N/Z match the narrow result; its low byte
                // still supplies PF and the original clear-AF contract.
                Narrow(1, value, width, true);
                LogicalFlags(1, 64, all_flags, true);
                saved = true;
                break;
            }
            if (NativeNZCV() && operation.opcode == OpCode::And &&
                (flag_mask == zero || flag_mask == (carry | zero)) &&
                input.Kind() == OperandKind::Value && home.last_use == home.definition_position &&
                (operand.Kind() == OperandKind::Immediate ||
                 (operand.Kind() == OperandKind::Value && input.Payload() == operand.Payload() &&
                  View(input) == View(operand)))) {
                auto tested_bits = std::min(width, Width(View(input)));
                tested_bits =
                        std::min(tested_bits, allocation.homes[input.Payload()].CarrierBits());
                const auto test_width = tested_bits <= 32 ? 32u : 64u;
                const bool immediate = operand.Kind() == OperandKind::Immediate;
                const bool immediate_test = immediate || tested_bits < 32;
                LogicalMask mask;
                u64 tested_mask{};
                bool encodable = true;
                if (immediate) {
                    const auto literal_bits = std::min(tested_bits, Width(operand.TypeView()));
                    const auto literal =
                            IntegerImmediate(operand) & (~u64{} >> (64 - literal_bits));
                    tested_mask = literal;
                    encodable = mask.Encode(literal, test_width);
                } else if (immediate_test) {
                    tested_mask = (u64{1} << tested_bits) - 1;
                    mask.Encode(tested_mask, test_width);
                }
                if (encodable) {
                    cursor.Take();
                    const auto value = RawValue(static_cast<u32>(input.Payload()), 16, true);
                    nzcv_matches = 0;
                    PrepareNZCV(flag_mask);
                    if (immediate_test)
                        mask.Emit(masm.exact, R(31, test_width), R(value, test_width), v::ANDS);
                    else
                        masm.exact.tst(R(value, test_width), Operand(R(value, test_width)));
                    FinishNZCV(flag_mask,
                               true);  // TST's C=0 needs the native inverted guest carry.
                    saved = true;      // No numerical reader exists, including a spill reader.
                    break;
                }
            }
            if (NativeNZCV() && operation.opcode == OpCode::Sub && (width == 8 || width == 16) &&
                !legacy_mask && (flag_mask == zero || flag_mask == (carry | zero)) &&
                input.Kind() == OperandKind::Value && home.last_use == home.definition_position) {
                u64 literal{};
                bool shifted{}, compare{};
                if (operand.Kind() == OperandKind::Immediate) {
                    const auto bits = std::min(width, Width(operand.TypeView()));
                    literal = IntegerImmediate(operand) & (~u64{} >> (64 - bits));
                    shifted = literal > 4095 && !(literal & 4095);
                    compare = (shifted ? literal >> 12 : literal) <= 4095;
                } else if (operand.Kind() == OperandKind::Value) {
                    // Match Read's already-normalized numeric case. A pair
                    // of dirty borrowed pins can be cheaper in the aligned
                    // fallback than two cuts followed by a W comparison.
                    const auto canonical = [&](CellView value) {
                        const auto bits = std::min(width, Width(View(value)));
                        const auto& producer = source.Operations()[value.Payload()];
                        return !allocation.homes[value.Payload()].narrow_pin &&
                               Bits(producer.type) <= bits && producer.opcode != OpCode::BitCast &&
                               producer.opcode != OpCode::GetResult &&
                               producer.opcode != OpCode::GetFlags;
                    };
                    compare = canonical(input) && canonical(operand);
                }
                if (compare) {
                    cursor.Take();
                    const auto right = operand.Kind() == OperandKind::Value
                                               ? Read(operand, 17, width, false, 32)
                                               : 31u;
                    const auto left = Read(input, 16, width, false, 32);
                    nzcv_matches = 0;
                    PrepareNZCV(flag_mask);
                    if (operand.Kind() == OperandKind::Immediate)
                        masm.exact.cmp(W(left), Operand(literal));
                    else
                        masm.exact.cmp(W(left), Operand(W(right)));
                    // Zero-extended narrow inputs have the same C/Z as CMP W.
                    // The ordinary flags path preserves N/V and the raw PF/AF.
                    ArithmeticFlags(31,
                                    flag_mask,
                                    metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                    saved = true;
                    break;
                }
            }
            if (RawAF() && operation.opcode == OpCode::Sub && (width == 8 || width == 16) &&
                !legacy_mask && flag_mask == all_flags && input.Kind() == OperandKind::Value &&
                operand.Kind() == OperandKind::Immediate &&
                home.last_use == home.definition_position) {
                const auto bits = std::min(width, Width(operand.TypeView()));
                const auto literal = IntegerImmediate(operand) & (~u64{} >> (64 - bits));
                const bool negative_literal = literal & (u64{1} << (width - 1));
                const auto magnitude = negative_literal ? (u64{1} << width) - literal : literal;
                const bool shifted = magnitude > 4095 && !(magnitude & 4095);
                const auto field = shifted ? magnitude >> 12 : magnitude;
                if (field <= 4095) {
                    cursor.Take();
                    const auto input_bits = std::min(
                            Width(View(input)), allocation.homes[input.Payload()].CarrierBits());
                    const auto left =
                            input_bits >= width
                                    ? RawValue(static_cast<u32>(input.Payload()), 16, true)
                                    : Read(input, 16, width, false, 32);
                    masm.exact.sbfx(
                            v::w16, W(left), 0, width);  // SXTB/SXTH W16 also cuts dirty pins.
                    // Raw AF only observes bit4. A W write also clears the TEST tag.
                    if (literal & 16)
                        masm.exact.mvn(v::w29, Operand(v::w16));
                    else
                        masm.exact.mov(v::w29, v::w16);
                    nzcv_matches = 0;
                    PrepareNZCV(flag_mask);
                    if (negative_literal)
                        masm.exact.adds(v::w1, v::w16, Operand(magnitude));
                    else
                        masm.exact.subs(v::w1, v::w16, Operand(magnitude));
                    // Signed narrow subtraction fits n+1 bits. SETF restores
                    // N/Z/V from that result and preserves the correct unsigned C.
                    if (width == 8)
                        masm.exact.setf8(v::w1);
                    else
                        masm.exact.setf16(v::w1);
                    ArithmeticFlags(1,
                                    flag_mask,
                                    metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                    saved = true;  // The raw PF carrier's high bits are not a numerical result.
                    break;
                }
            }
            if (operation.opcode == OpCode::And && operand.Kind() == OperandKind::Immediate &&
                (width == 8 || width == 16)) {
                const auto literal_bits = std::min(width, Width(operand.TypeView()));
                const auto literal = IntegerImmediate(operand) & ((u64{1} << literal_bits) - 1);
                const auto repeated = width == 8 ? literal * 0x01010101u : literal * 0x00010001u;
                LogicalMask mask;
                if (mask.Encode(repeated, 32)) {
                    cursor.Take();
                    // Repetition only encodes the mask. Read still clears
                    // every input bit above the logical width, including
                    // borrowed pins and raw transports. The W result stays
                    // canonical, without a separate result cut.
                    const auto left = Read(input, 16, width, false, 32);
                    mask.Emit(masm.exact, W(destination), W(left), v::AND);
                    if (flag_mask) {
                        save_result();
                        LogicalFlags(destination, width, flag_mask, !legacy_mask);
                    }
                    break;
                }
            }
            if (Logical(operation.opcode) && operand.Kind() == OperandKind::Immediate &&
                width >= 32) {
                auto literal = IntegerImmediate(operand);
                const auto literal_width = std::min(Width(operand.TypeView()), width);
                if (literal_width < 64) literal &= (u64{1} << literal_width) - 1;
                if (operation.opcode == OpCode::AndNot) literal = ~literal;
                LogicalMask mask;
                if (mask.Encode(literal, width)) {
                    cursor.Take();
                    const auto left = Read(input, 16, 64, false, native_width);
                    const bool set_nzcv = (operation.opcode == OpCode::And ||
                                           operation.opcode == OpCode::AndNot) &&
                                          (flag_mask & (negative | zero));
                    if (set_nzcv) {
                        nzcv_matches = 0;
                        PrepareNZCV(flag_mask);
                    }
                    const auto result = R(destination, width), source_reg = R(left, width);
                    if (operation.opcode == OpCode::Or)
                        mask.Emit(masm.exact, result, source_reg, v::ORR);
                    else if (operation.opcode == OpCode::Xor)
                        mask.Emit(masm.exact, result, source_reg, v::EOR);
                    else if (set_nzcv)
                        mask.Emit(masm.exact, result, source_reg, v::ANDS);
                    else
                        mask.Emit(masm.exact, result, source_reg, v::AND);
                    if (flag_mask) {
                        save_result();
                        LogicalFlags(destination, width, flag_mask, !legacy_mask, set_nzcv);
                    }
                    break;
                }
            }
            if (Arithmetic(operation.opcode) && operand.Kind() == OperandKind::Immediate &&
                (width >= 32 || ((width == 8 || width == 16) && !flag_mask))) {
                auto literal = IntegerImmediate(operand);
                const auto literal_width = std::min(Width(operand.TypeView()), width);
                if (literal_width < 64) literal &= (u64{1} << literal_width) - 1;
                bool subtract = operation.opcode == OpCode::Sub;
                if (!flag_mask && (literal & (u64{1} << (width - 1)))) {
                    literal = (-literal) & (~u64{} >> (64 - width));
                    subtract = !subtract;
                }
                const bool shifted = literal > 4095 && !(literal & 4095);
                const auto field = shifted ? literal >> 12 : literal;
                if (field <= 4095) {
                    cursor.Take();
                    // A final narrow result makes the low-width sum modulo
                    // 2^width. Borrow only when the actual read is at least
                    // that wide; Bool/narrower views still use numeric Read.
                    const bool raw_narrow =
                            width < 32 && input.Kind() == OperandKind::Value &&
                            std::min(Width(View(input)),
                                     allocation.homes[input.Payload()].CarrierBits()) >= width;
                    const auto left =
                            raw_narrow ? RawValue(static_cast<u32>(input.Payload()), 16, true)
                                       : Read(input, 16, 64, false, width <= 32 ? 32u : 64u);
                    if (flag_mask & auxiliary) {
                        // ArithmeticFlags consumes only (left ^ right).bit4.
                        // Capture it before a destination pin overwrites left.
                        if (RawAF()) {
                            if (literal & 16)
                                masm.exact.eor(v::w29, W(left), Operand(16));  // EOR W29,Wleft,#16.
                            else
                                masm.exact.mov(v::w29, W(left));
                        } else if (literal & 16)
                            masm.exact.eor(v::x0, X(left), Operand(16));  // EOR x0,left,#16.
                        else
                            Move(0, left);
                    }
                    const bool set_nzcv = flag_mask & nzcv_flags;
                    if (set_nzcv) {
                        nzcv_matches = 0;
                        PrepareNZCV(flag_mask);
                    }
                    const auto result = R(destination, width <= 32 ? 32 : 64),
                               source_reg = R(left, result.GetSizeInBits());
                    if (subtract) {
                        if (set_nzcv)
                            masm.exact.subs(result, source_reg, Operand(literal));
                        else
                            masm.exact.sub(result, source_reg, Operand(literal));
                    } else {
                        if (set_nzcv)
                            masm.exact.adds(result, source_reg, Operand(literal));
                        else
                            masm.exact.add(result, source_reg, Operand(literal));
                    }
                    const bool partial_byte_store = !flag_mask && operation.type == Type::U8 &&
                                                    metadata == OperationMetadata{} &&
                                                    PartialByteStore(operation, home);
                    if (width < 32 && !partial_byte_store) Narrow(destination, destination, width);
                    if (flag_mask) {
                        save_result();
                        ArithmeticFlags(destination,
                                        flag_mask,
                                        subtract && metadata.profile ==
                                                            FlagProfile::ArithmeticCanonicalCarry);
                    }
                    break;
                }
            }
            const bool logical_nzcv =
                    (operation.opcode == OpCode::And || operation.opcode == OpCode::AndNot) &&
                    width >= 32 && (flag_mask & (negative | zero));
            const bool set_nzcv =
                    logical_nzcv || (Arithmetic(operation.opcode) && (flag_mask & nzcv_flags));
            const bool aligned = set_nzcv && width < 32;
            const bool zero_right =
                    operand.Kind() == OperandKind::Immediate &&
                    !(IntegerImmediate(operand) &
                      (~u64{} >> (64 - std::min(width, Width(operand.TypeView())))));
            unsigned right, left;
            bool physical_logical{};
            if (width < 32 &&
                (Logical(operation.opcode) || (aligned && (width == 8 || width == 16))) &&
                (operand.Kind() == OperandKind::Value ||
                 operand.Kind() == OperandKind::Immediate)) {
                const auto read_low = [&](CellView value, unsigned scratch) {
                    if (value.Kind() == OperandKind::Value) {
                        const auto& input_home = allocation.homes[value.Payload()];
                        // Aligned UBFIZ and the shifted W ALU cut their own inputs.
                        if ((aligned || input_home.narrow_pin) &&
                            std::min(Width(View(value)), input_home.CarrierBits()) >= width) {
                            physical_logical = true;
                            return RawValue(static_cast<u32>(value.Payload()), scratch, true);
                        }
                    }
                    return Read(value, scratch);
                };
                cursor.Take();
                right = zero_right ? 31u : read_low(operand, 17);
                left = read_low(input, 16);
            } else {
                if (zero_right) {
                    cursor.Take();
                    right = 31;
                } else
                    right = Expression(cursor, 17, 16, native_width);
                left = Read(input, 16, 64, false, native_width);
            }
            // Ordinary narrow reads already supplied canonical integers.
            // A borrowed physical input instead needs one result cut;
            // compound expressions and wider reads retain theirs too.
            const bool normalized_logical =
                    !physical_logical && width < 32 && Logical(operation.opcode) &&
                    (operand.Kind() == OperandKind::Value ||
                     operand.Kind() == OperandKind::Immediate) &&
                    Width(View(input)) <= width && Width(View(operand)) <= width;
            const bool partial_byte_store = physical_logical && RawAF() && !legacy_mask &&
                                            operation.opcode == OpCode::Xor &&
                                            flag_mask == all_flags &&
                                            metadata.profile == FlagProfile::LogicalZeroCVA &&
                                            PartialByteStore(operation, home);
            if (flag_mask & auxiliary && Arithmetic(operation.opcode))
                masm.exact.eor(R(RawAF() ? 29 : 0, RawAF() ? 32 : 64),
                               R(left, RawAF() ? 32 : 64),
                               Operand(R(right, RawAF() ? 32 : 64)));
            if (aligned) {
                // A W shift drops bits above the logical arithmetic width;
                // the RHS shift fits the actual ADDS/SUBS operand field.
                masm.exact.lsl(v::w16, W(left), 32 - width);
                left = 16;
            }
            const auto alu_width = width == 32 || aligned ? 32u : 64u;
            const auto result = R(destination, alu_width), left_reg = R(left, alu_width);
            const Operand right_reg(R(right, alu_width), v::LSL, aligned ? 32 - width : 0);
            if (set_nzcv) {
                nzcv_matches = 0;
                PrepareNZCV(flag_mask);
            }
            switch (operation.opcode) {
                case OpCode::Add:
                    if (set_nzcv)
                        masm.exact.adds(result, left_reg, right_reg);
                    else
                        masm.exact.add(result, left_reg, right_reg);
                    break;
                case OpCode::Sub:
                    if (set_nzcv)
                        masm.exact.subs(result, left_reg, right_reg);
                    else
                        masm.exact.sub(result, left_reg, right_reg);
                    break;
                case OpCode::And:
                    if (set_nzcv)
                        masm.exact.ands(result, left_reg, right_reg);
                    else
                        masm.exact.and_(result, left_reg, right_reg);
                    break;
                case OpCode::Or:
                    masm.exact.orr(result, left_reg, right_reg);
                    break;
                case OpCode::Xor:
                    masm.exact.eor(result, left_reg, right_reg);
                    break;
                case OpCode::AndNot:
                    if (set_nzcv)
                        masm.exact.bics(result, left_reg, right_reg);
                    else
                        masm.exact.bic(result, left_reg, right_reg);
                    break;
                default:
                    Reject(CompileError::UnsupportedOpcode);
            }
            if (aligned) Extract(destination, destination, 32 - width, width);
            // Here the only numeric reader is the adjacent byte insertion;
            // it and LogicalFlags' SXTB each cut their own actual low eight bits.
            else if (width != 32 && !normalized_logical && !partial_byte_store)
                Narrow(destination, destination, width);
            if (flag_mask) {
                save_result();
                if (Arithmetic(operation.opcode))
                    ArithmeticFlags(
                            destination,
                            flag_mask,
                            operation.opcode == OpCode::Sub &&
                                    metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                else
                    LogicalFlags(destination, width, flag_mask, !legacy_mask, logical_nzcv);
            }
            break;
        }
    } while (false);
}

void Writer::HandleNegFamily(OpContext& ctx) {
    const auto& metadata = ctx.metadata;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    const auto flag_mask = ctx.flag_mask;
    const auto save_result = [&] { SaveResult(ctx); };
    do {
        {
            auto value = Read(cursor.Take(), 16, 64, false, width == 32 ? 32 : 64);
            if (flag_mask & auxiliary) {
                if (RawAF())
                    masm.exact.mov(v::w29, W(value));
                else
                    Move(0, value);
            }
            const bool set_nzcv = flag_mask & (carry | overflow | zero | negative);
            const bool aligned = set_nzcv && width < 32;
            if (aligned) {
                Narrow(16, value, width);
                ShiftLeft(16, 16, 32 - width);
                value = 16;
            }
            const auto alu_width = set_nzcv ? (width <= 32 ? 32u : 64u) : width == 32 ? 32u : 64u;
            if (set_nzcv) {
                nzcv_matches = 0;
                PrepareNZCV(flag_mask);
            }
            if (set_nzcv)
                masm.exact.negs(R(destination, alu_width), Operand(R(value, alu_width)));
            else
                masm.exact.neg(R(destination, alu_width), Operand(R(value, alu_width)));
            if (aligned)
                Extract(destination, destination, 32 - width, width);
            else if (width != 32)
                Narrow(destination, destination, width);
            if (flag_mask) {
                save_result();
                ArithmeticFlags(destination,
                                flag_mask,
                                metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
            }
            break;
        }
    } while (false);
}

void Writer::HandleAdcFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    const auto& metadata = ctx.metadata;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    const auto flag_mask = ctx.flag_mask;
    const auto save_result = [&] { SaveResult(ctx); };
    do {
        {
            RequireFlags();
            if (RawAF()) {
                const bool subtract = operation.opcode == OpCode::Sbb;
                const auto input = cursor.Take();
                const auto native_width = width == 32 ? 32u : 64u;
                auto right = Expression(cursor, 17, 16, native_width),
                     left = Read(input, 16, 64, false, native_width);
                if (width < 32) {
                    Narrow(16, left, width);
                    Narrow(17, right, width);
                    left = 16;
                    right = 17;
                }
                if (flag_mask & auxiliary) masm.exact.eor(v::w29, W(left), Operand(W(right)));
                if (width < 32 && (flag_mask & overflow)) {
                    masm.exact.eor(v::x0, X(left), Operand(X(right)));
                    Move(30, left);
                }
                const bool host_flags = width >= 32 && (flag_mask & nzcv_flags);
                if (host_flags) PrepareNZCV(flag_mask);
                if (!subtract)
                    masm.exact.cfinv();  // ADC consumes guest C, rather than its native inverse.
                if (subtract) {
                    if (host_flags)
                        masm.exact.sbcs(R(destination, native_width),
                                        R(left, native_width),
                                        Operand(R(right, native_width)));
                    else
                        masm.exact.sbc(R(destination, native_width),
                                       R(left, native_width),
                                       Operand(R(right, native_width)));
                } else {
                    if (host_flags)
                        masm.exact.adcs(R(destination, native_width),
                                        R(left, native_width),
                                        Operand(R(right, native_width)));
                    else
                        masm.exact.adc(R(destination, native_width),
                                       R(left, native_width),
                                       Operand(R(right, native_width)));
                }
                if (!subtract && !host_flags) masm.exact.cfinv();
                if (width < 32) {
                    // Non-S ADC/SBC preserve incoming NZCV. Publish only
                    // the requested narrow fields, without a packed bridge.
                    if (flag_mask & carry) {
                        Extract(17, destination, subtract ? 63 : width, 1);
                        Rmif(17, 63, 2);
                        if (!subtract || metadata.profile == FlagProfile::ArithmeticCanonicalCarry)
                            masm.exact.cfinv();
                    }
                    if (flag_mask & overflow) {
                        masm.exact.eor(v::x30, v::x30, Operand(X(destination)));
                        if (subtract)
                            masm.exact.and_(v::x30, v::x30, Operand(v::x0));
                        else
                            masm.exact.bic(v::x30, v::x30, Operand(v::x0));
                        Extract(17, 30, width - 1, 1);
                        Rmif(17, 0, 1);
                    }
                    Narrow(destination, destination, width);
                    save_result();
                    ArithmeticFlags(destination, flag_mask & (auxiliary | parity), false);
                    LogicalFlags(destination, width, flag_mask & (negative | zero), false);
                } else if (flag_mask) {
                    save_result();
                    ArithmeticFlags(
                            destination,
                            flag_mask,
                            subtract && metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                }
                break;
            }
            const bool native = NativeNZCV();
            PackNZCV();
            options.native_nzcv = false;  // The existing narrow emitter also owns x30.
            nzcv_matches = 0;
            const bool subtract = operation.opcode == OpCode::Sbb;
            const auto input = cursor.Take();
            const auto right = Expression(cursor, 17, 16), left = Read(input, 16);
            if (subtract) {
                masm.exact.eor(v::x0, v::x1, Operand(u64{1} << 29));  // EOR x0,x1,#(1<<29).
                masm.exact.msr(v::NZCV, v::x0);  // SBC consumes !borrow in host C.
            } else
                masm.exact.msr(v::NZCV, v::x1);
            if ((flag_mask & auxiliary) || (width < 32 && (flag_mask & overflow)))
                masm.exact.eor(v::x0, X(left), Operand(X(right)));
            if (width < 32 && (flag_mask & overflow)) Move(30, left);
            const bool host_flags =
                    width >= 32 && (flag_mask & (carry | overflow | zero | negative));
            const auto alu_width = width == 32 ? 32u : 64u;
            if (subtract) {
                if (host_flags)
                    masm.exact.sbcs(R(destination, alu_width),
                                    R(left, alu_width),
                                    Operand(R(right, alu_width)));
                else
                    masm.exact.sbc(R(destination, alu_width),
                                   R(left, alu_width),
                                   Operand(R(right, alu_width)));
            } else {
                if (host_flags)
                    masm.exact.adcs(R(destination, alu_width),
                                    R(left, alu_width),
                                    Operand(R(right, alu_width)));
                else
                    masm.exact.adc(R(destination, alu_width),
                                   R(left, alu_width),
                                   Operand(R(right, alu_width)));
            }
            if (width < 32 && flag_mask) {
                if (flag_mask & carry) {
                    Extract(17, destination, subtract ? 63 : width, 1);
                    if (subtract && metadata.profile != FlagProfile::ArithmeticCanonicalCarry) {
                        masm.exact.mvn(v::x17, Operand(v::x17));  // MVN; only bit zero is inserted.
                    }
                    Insert(1, 17, 29, 1);
                }
                if (flag_mask & overflow) {
                    masm.exact.eor(v::x30, v::x30, Operand(X(destination)));
                    if (subtract)
                        masm.exact.and_(v::x30, v::x30, Operand(v::x0));
                    else
                        masm.exact.bic(v::x30, v::x30, Operand(v::x0));
                    Extract(17, 30, width - 1, 1);
                    Insert(1, 17, 28, 1);
                }
                if (flag_mask & auxiliary) {
                    masm.exact.eor(v::x0, v::x0, Operand(X(destination)));
                    Extract(0, 0, 4, 1);
                    Insert(1, 0, 26, 1);
                }
                Narrow(destination, destination, width);
                save_result();
                LogicalFlags(destination, width, flag_mask & (zero | negative | parity), false);
            } else {
                Narrow(destination, destination, width);
                if (flag_mask) {
                    save_result();
                    ArithmeticFlags(
                            destination,
                            flag_mask,
                            subtract && metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                }
            }
            nzcv_matches =
                    0;  // Narrow C/V are synthesized separately; keep this case conservative.
            options.native_nzcv = native;
            RestoreNZCV();
            break;
        }
    } while (false);
}

void Writer::HandleDiv128Family(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    do {
        {
            const auto high = cursor.Take(), low = cursor.Take(), divisor = cursor.Take();
            const auto sign = IntegerImmediate(cursor.Take());
            const auto bits = Width(View(high));
            if ((bits != 8 && bits != 16 && bits != 32 && bits != 64) || Width(View(low)) != bits ||
                Width(View(divisor)) != bits || sign > 1)
                Reject(CompileError::UnsupportedOperand);
            Shadow(false);
            PublishPinned();
            HelperFPEnvironment(false);
            Move(0, Read(high, 0));
            Move(16, Read(low, 16));
            Move(17, Read(divisor, 17));
            Immediate(30, DivisionHelper(bits, sign != 0));
            masm.exact.blr(v::x30);  // BLR x30.
            HelperFPEnvironment(true);
            nzcv_matches = 0;
            Memory(false, 64, 16, 31, home.extra_slot - 1);
            Move(destination, 0);
            RestoreHelperNZCV();
            break;
        }
    } while (false);
}

void Writer::HandleDiv128RemainderFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    do {
        {
            const auto owner = cursor.Take();
            if (owner.Kind() != OperandKind::Value ||
                source.Operations()[owner.Payload()].opcode != OpCode::Div128)
                Reject(CompileError::UnsupportedOperand);
            Memory(true, 64, destination, 31, allocation.homes[owner.Payload()].extra_slot - 1);
            break;
        }
    } while (false);
}

void Writer::HandleCpuidFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    do {
        {
            const auto leaf = cursor.Take(), subleaf = cursor.Take();
            const auto callback = IntegerImmediate(cursor.Take());
            if (!callback) Reject(CompileError::UnsupportedOperand);
            Shadow(false);
            PublishPinned();
            masm.exact.dmb(
                    v::InnerShareable,
                    v::BarrierAll);  // DMB ISH: prior guest memory precedes the serializing query.
            HelperFPEnvironment(false);
            Move(0, Read(leaf, 0));
            Move(16, Read(subleaf, 16));
            Immediate(17, callback);
            Immediate(30, CpuidHelper());
            masm.exact.blr(v::x30);  // BLR x30.
            HelperFPEnvironment(true);
            masm.exact.isb();  // ISB: following instructions start after the query.
            nzcv_matches = 0;
            Memory(false, 64, 16, 31, home.extra_slot - 1);
            Move(destination, 0);
            RestoreHelperNZCV();
            break;
        }
    } while (false);
}

void Writer::HandleCpuidUpperFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    do {
        {
            const auto owner = cursor.Take();
            if (owner.Kind() != OperandKind::Value ||
                source.Operations()[owner.Payload()].opcode != OpCode::Cpuid)
                Reject(CompileError::UnsupportedOperand);
            Memory(true, 64, destination, 31, allocation.homes[owner.Payload()].extra_slot - 1);
            break;
        }
    } while (false);
}

void Writer::HandleMulFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    const auto& metadata = ctx.metadata;
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    auto& saved = ctx.saved;
    do {
        {
            const auto input = cursor.Take();
            const auto operand = *cursor.at;
            if (operation.type == Type::U32 && metadata == OperationMetadata{} && !home.spilled &&
                node.value + 1 < source.Operations().size()) {
                const NodeId consumer{node.value + 1};
                const auto& next = source.Operations()[consumer.value];
                const auto& next_home = allocation.homes[consumer.value];
                if (next.opcode == OpCode::Add && next.type == Type::U32 &&
                    next.block == operation.block && next.Metadata() == OperationMetadata{} &&
                    !next_home.spilled && home.last_use == next_home.definition_position &&
                    next_home.reg != deferred_parity) {
                    const auto addends = source.Operands(consumer);
                    auto at = addends.begin();
                    if (at != addends.end()) {
                        const auto first = *at++;
                        if (at != addends.end()) {
                            const auto second = *at++;
                            const bool first_product = first.Kind() == OperandKind::Value &&
                                                       first.Payload() == node.value;
                            const bool second_product = second.Kind() == OperandKind::Value &&
                                                        second.Payload() == node.value;
                            const auto accumulator = first_product ? second : first;
                            const auto register_input = [&](CellView value) {
                                return value.Kind() == OperandKind::Value &&
                                       View(value) == Type::U32 &&
                                       !allocation.homes[value.Payload()].spilled &&
                                       allocation.homes[value.Payload()].CarrierBits() >= 32;
                            };
                            NodeId publication{consumer.value + 1};
                            if (publication.value < source.Operations().size() &&
                                source.Operations()[publication.value].opcode == OpCode::ClearFlags)
                                ++publication.value;
                            if (at == addends.end() && first_product != second_product &&
                                View(first_product ? first : second) == Type::U32 &&
                                register_input(input) && register_input(operand) &&
                                register_input(accumulator) && !LegacyMask(consumer, publication)) {
                                cursor.Take();
                                const auto left = Read(input, 16, 64, false, 32);
                                const auto right = Read(operand, 17, 64, false, 32);
                                const auto addend = Read(accumulator, 16, 64, false, 32);
                                masm.exact.madd(W(next_home.reg), W(left), W(right), W(addend));
                                elided_consumer = consumer;
                                saved = true;
                                break;
                            }
                        }
                    }
                }
            }
            const auto native_width = width <= 32 ? 32u : 64u;
            const auto right = Expression(cursor, 17, 16, native_width),
                       left = Read(input, 16, 64, false, native_width);
            masm.exact.mul(
                    R(destination, native_width), R(left, native_width), R(right, native_width));
            if (width < 32) Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

void Writer::HandleMulHighFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto left = RawScalar(cursor.Take(), 16), right = RawScalar(cursor.Take(), 17);
            const auto sign = IntegerImmediate(cursor.Take());
            if (sign > 1) Reject(CompileError::UnsupportedOperand);
            if (sign)
                masm.exact.smulh(X(destination), X(left), X(right));
            else
                masm.exact.umulh(X(destination), X(left), X(right));
            Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

void Writer::HandleBitExtractFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = cursor.Take();
            const auto first = IntegerImmediate(cursor.Take()),
                       count = IntegerImmediate(cursor.Take());
            Field(first, count);
            if (!count && operation.opcode == OpCode::BitExtract)
                Immediate(destination, 0);
            else {
                const bool extract = operation.opcode == OpCode::BitExtract;
                const auto value = RawScalar(
                        input, 16, extract ? 1u : 0u);  // Extract clips its carrier range below.
                if (extract) {
                    // Borrowed pin bits above the logical carrier are zero;
                    // UBFX supplies both the extraction and result narrowing.
                    const auto carrier_bits = allocation.homes[input.Payload()].CarrierBits();
                    const auto remaining = first < carrier_bits ? u64{carrier_bits} - first : 0;
                    const auto actual_count = std::min({count, u64{width}, remaining});
                    if (actual_count)
                        Extract(destination,
                                value,
                                static_cast<unsigned>(first),
                                static_cast<unsigned>(actual_count));
                    else
                        Immediate(destination, 0);
                } else {
                    Move(destination, value);
                    if (count)
                        Insert(destination,
                               31,
                               static_cast<unsigned>(first),
                               static_cast<unsigned>(count));
                    Narrow(destination, destination, width);
                }
            }
            break;
        }
    } while (false);
}

void Writer::HandleBitInsertFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = cursor.Take(), insertion = cursor.Take();
            const auto first = IntegerImmediate(cursor.Take()),
                       count = IntegerImmediate(cursor.Take());
            Field(first, count);
            const bool word_insert = width == 32 && count && first < 32 && count <= 32 - first;
            const auto value = RawScalar(input, 16, word_insert ? 32u : 0u);
            Width(View(insertion));
            auto other =
                    count ? RawScalar(
                                    insertion, 17, word_insert ? static_cast<unsigned>(count) : 0u)
                          : 31;
            if (other == destination && value != destination) {
                Move(17, other);  // Preserve the whole raw source before copying the parent over
                                  // its last-use home.
                other = 17;
            }
            Move(destination, value);
            if (count)
                Insert(destination,
                       other,
                       static_cast<unsigned>(first),
                       static_cast<unsigned>(count),
                       word_insert ? 32u : 64u);
            if (!word_insert) Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

void Writer::HandleCrc32cFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    do {
        {
            if (!options.crc32c) Reject(CompileError::UnsupportedOpcode);
            const auto accumulator = cursor.Take(), input = cursor.Take();
            const auto bits = IntegerImmediate(cursor.Take());
            if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
                Reject(CompileError::UnsupportedOperand);
            const auto initial = Read(accumulator, 16, 32, false, 32);
            const auto data =
                    Read(input, 17, static_cast<unsigned>(bits), false, bits == 64 ? 64 : 32);
            // CRC32CB/CH/CW/CX: W accumulator and result, with X data
            // only for the 64-bit form. No NZCV or complement operation.
            if (bits == 8)
                masm.exact.crc32cb(W(destination), W(initial), W(data));
            else if (bits == 16)
                masm.exact.crc32ch(W(destination), W(initial), W(data));
            else if (bits == 32)
                masm.exact.crc32cw(W(destination), W(initial), W(data));
            else
                masm.exact.crc32cx(W(destination), W(initial), X(data));
            break;
        }
    } while (false);
}

void Writer::HandleByteSwapFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = RawScalar(cursor.Take(), 16);
            const auto swapped = IntegerImmediate(cursor.Take());
            if (swapped != 16 && swapped != 32 && swapped != 64)
                Reject(CompileError::UnsupportedOperand);
            if (swapped == 16)
                masm.exact.rev16(W(destination), W(input));
            else
                masm.exact.rev(R(destination, swapped), R(input, swapped));
            const auto kept = std::min(width, static_cast<unsigned>(swapped));
            if (kept < 32 || (swapped == 64 && kept < 64)) Narrow(destination, destination, kept);
            break;
        }
    } while (false);
}

void Writer::HandlePopCountFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = RawScalar(cursor.Take(), 16);
            // Both scalar and vector allocation reserve v0 as scratch;
            // it is never a value home.
            masm.exact.fmov(v::d0, X(input));  // Raw bits, no FP conversion.
            masm.exact.cnt(Lanes(0, 8, 64), Lanes(0, 8, 64));
            masm.exact.addv(v::b0, Lanes(0, 8, 64));
            masm.exact.umov(W(destination), Lanes(0, 8, 64), 0);
            if (width < 7) Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

void Writer::HandleCountLeadingZeros32Family(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const bool wide = operation.opcode == OpCode::CountLeadingZeros64 ||
                              operation.opcode == OpCode::CountTrailingZeros64;
            const bool trailing = operation.opcode == OpCode::CountTrailingZeros32 ||
                                  operation.opcode == OpCode::CountTrailingZeros64;
            const auto input = cursor.Take();
            if (Width(View(input)) != (wide ? 64u : 32u)) Reject(CompileError::UnsupportedType);
            auto value = RawScalar(input, 16);
            if (trailing) {
                masm.exact.rbit(R(destination, wide ? 64 : 32), R(value, wide ? 64 : 32));
                value = destination;
            }
            masm.exact.clz(R(destination, wide ? 64 : 32), R(value, wide ? 64 : 32));
            if (width < (wide ? 7u : 6u)) Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

void Writer::HandleNotFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = cursor.Take();
            if (cursor.at == cursor.end)
                Predicate(destination, Read(input, 16), true);
            else {
                const auto value = Expression(cursor, 16, 17);
                masm.exact.mvn(X(destination), Operand(X(value)));
            }
            Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

void Writer::HandleTestZeroFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    do {
        Predicate(destination, Read(cursor.Take(), 16), operation.opcode == OpCode::TestZero);
        break;
    } while (false);
}

void Writer::HandleTestBitFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    do {
        {
            const auto input = cursor.Take();
            const auto bit = IntegerImmediate(cursor.Take());
            if (bit >= 64) Reject(CompileError::UnsupportedOperand);
            if (bit >= Width(View(input)))
                Immediate(destination, 0);
            else
                Extract(destination, Read(input, 16), static_cast<unsigned>(bit), 1);
            break;
        }
    } while (false);
}

void Writer::HandleLslImmFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
        {
            const auto input = cursor.Take(), amount = cursor.Take();
            const bool asr =
                    operation.opcode == OpCode::AsrImm || operation.opcode == OpCode::AsrValue;
            const bool ror =
                    operation.opcode == OpCode::RorImm || operation.opcode == OpCode::RorValue;
            const bool lsl =
                    operation.opcode == OpCode::LslImm || operation.opcode == OpCode::LslValue;
            if (ror && width != 32 && width != 64) Reject(CompileError::UnsupportedType);
            if (amount.Kind() == OperandKind::Immediate) {
                const auto count = static_cast<unsigned>(
                        IntegerImmediate(amount) & (width - 1) &
                        ((u64{1} << std::min(Width(amount.TypeView()), 6u)) - 1));
                const auto value = Read(input, 16, width, asr, width == 32 ? 32 : 64);
                if (ror) {
                    masm.exact.ror(R(destination, width), R(value, width), count);
                } else if (asr) {
                    masm.exact.asr(R(destination, width == 32 ? 32 : 64),
                                   R(value, width == 32 ? 32 : 64),
                                   count);
                    if (width < 32) Narrow(destination, destination, width);
                } else {
                    const auto register_width = width == 32 ? 32u : 64u;
                    masm.exact.ubfm(R(destination, register_width),
                                    R(value, register_width),
                                    lsl ? (register_width - count) % register_width : count,
                                    lsl ? width - count - 1 : width - 1);
                }
                break;
            }
            auto count = Read(amount, 17);
            // Scalar shift counts reduce modulo the actual result width.
            unsigned count_bits{};
            for (auto n = width; n > 1; n >>= 1) ++count_bits;
            if (!count_bits) {
                Immediate(17, 0);
                count = 17;
            } else {
                Narrow(17, count, count_bits);
                count = 17;
            }
            const auto value = Read(input, 16, width, asr, width == 32 ? 32 : 64);
            const auto alu_width = width == 32 ? 32u : 64u;
            if (asr)
                masm.exact.asrv(
                        R(destination, alu_width), R(value, alu_width), R(count, alu_width));
            else if (ror)
                masm.exact.rorv(
                        R(destination, alu_width), R(value, alu_width), R(count, alu_width));
            else if (lsl)
                masm.exact.lslv(
                        R(destination, alu_width), R(value, alu_width), R(count, alu_width));
            else
                masm.exact.lsrv(
                        R(destination, alu_width), R(value, alu_width), R(count, alu_width));
            if (width < 32) Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64
