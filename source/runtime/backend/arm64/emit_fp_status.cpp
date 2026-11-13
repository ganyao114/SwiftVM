#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64::detail {

void Writer::HandleReadFPStatusFamily(OpContext& ctx) {
    Move(0, Read(ctx.cursor.Take(), 0));
    __ mrs(v::x30, v::FPSR);
    Extract(ctx.destination, 30, 0, 1);
    Extract(17, 30, 7, 1);
    __ orr(W(ctx.destination), W(ctx.destination), Operand(v::w17, v::LSL, 1));
    Extract(17, 30, 1, 4);
    __ orr(W(ctx.destination), W(ctx.destination), Operand(v::w17, v::LSL, 2));
    __ and_(v::w0, v::w0, Operand(u64{63}));
    __ orr(W(ctx.destination), W(ctx.destination), Operand(v::w0));
}

void Writer::LoadMxcsrFlushControl() {
    if (options.mxcsr_flush_mode != UINT8_MAX || options.mxcsr_flush_offset == UINT32_MAX) return;
    const auto offset = options.mxcsr_flush_offset;
    if (!(offset & 3) && offset / 4 <= 4095)
        Memory(true, 32, 0, 19, offset / 4);
    else {
        Immediate(17, offset);
        __ add(v::x17, v::x19, Operand(v::x17));
        Memory(true, 32, 0, 17);
    }
}

u32 Writer::DenormalControlGuard() {
    if (options.mxcsr_flush_mode != UINT8_MAX) return UINT32_MAX;
    if (options.mxcsr_flush_offset != UINT32_MAX) {
        const auto disabled = masm.Here();
        __ tbnz(v::w0, 6, std::int64_t{0});
        return disabled;
    }
    if (options.native_fp_environment) {
        // AFP keeps DAZ in FPCR.FIZ. Read once for the whole consumer.
        __ mrs(v::x17, v::FPCR);
        const auto disabled = masm.Here();
        __ tbnz(v::x17, 0, std::int64_t{0});
        return disabled;
    }
    return UINT32_MAX;
}

bool Writer::ProvenNoDenormal(CellView cell, unsigned bits, unsigned count) const {
    if (cell.Kind() != OperandKind::Value || cell.Payload() >= source.Operations().size())
        return false;
    auto id = static_cast<u32>(cell.Payload());
    for (unsigned depth = 0; depth < 4; ++depth) {
        const auto& cur = source.Operations()[id];
        if (cur.opcode != OpCode::BitCast || cur.type != Type::V128 ||
            cur.Metadata() != OperationMetadata{})
            break;
        const auto args = source.Operands(NodeId{id});
        if (args.begin() == args.end()) return false;
        const auto inner = args.front();
        if (inner.Kind() != OperandKind::Value || inner.Payload() >= source.Operations().size())
            return false;
        id = static_cast<u32>(inner.Payload());
    }
    const auto& op = source.Operations()[id];
    if (op.opcode == OpCode::VecSharedZero) return true;
    if (op.opcode == OpCode::VecFCvtPacked) {
        const auto args = source.Operands(NodeId{id});
        auto at = args.begin();
        if (at == args.end()) return false;
        ++at;
        if (at == args.end() || (*at).Kind() != OperandKind::Immediate) return false;
        const auto kind = (*at).Payload();
        return (kind == 0 && bits == 32) || ((kind == 1 || kind == 6) && bits == 64);
    }
    if (op.opcode == OpCode::VecFRoundInt) {
        const auto args = source.Operands(NodeId{id});
        auto at = args.begin();
        for (unsigned k = 0; k < 2 && at != args.end(); ++k) ++at;
        if (at == args.end() || (*at).Kind() != OperandKind::Immediate) return false;
        const auto lane_bits = (*at++).Payload();
        if (at == args.end()) return false;
        ++at;
        if (at == args.end() || (*at).Kind() != OperandKind::Immediate) return false;
        const auto scalar = (*at).Payload();
        return lane_bits == bits && (!scalar || count == 1);
    }
    u64 low{}, high{};
    if (op.opcode == OpCode::VecLoadConst) {
        const auto args = source.Operands(NodeId{id});
        auto at = args.begin();
        if (at == args.end()) return false;
        const auto first = *at++;
        if (first.Kind() != OperandKind::Immediate || at == args.end()) return false;
        const auto second = *at;
        if (second.Kind() != OperandKind::Immediate) return false;
        low = first.Payload();
        high = second.Payload();
    } else if (op.opcode == OpCode::VecScalar) {
        const auto args = source.Operands(NodeId{id});
        auto at = args.begin();
        if (at == args.end()) return false;
        const auto literal = *at;
        if (literal.Kind() == OperandKind::Value &&
            literal.Payload() < source.Operations().size()) {
            const auto scalar_id = static_cast<u32>(literal.Payload());
            const auto& scalar_op = source.Operations()[scalar_id];
            if (scalar_op.opcode == OpCode::VecFCvtIntToFloat) {
                const auto sargs = source.Operands(NodeId{scalar_id});
                auto sat = sargs.begin();
                for (unsigned k = 0; k < 2 && sat != sargs.end(); ++k) ++sat;
                return sat != sargs.end() && (*sat).Kind() == OperandKind::Immediate &&
                       (*sat).Payload() == bits;
            }
            if (scalar_op.opcode == OpCode::VecFCvtScalar && bits == 64) {
                const auto sargs = source.Operands(NodeId{scalar_id});
                auto sat = sargs.begin();
                if (sat != sargs.end()) ++sat;
                return sat != sargs.end() && (*sat).Kind() == OperandKind::Immediate &&
                       (*sat).Payload() == 32;
            }
            return false;
        }
        if (literal.Kind() != OperandKind::Immediate) return false;
        const auto view_bits = Bits(literal.TypeView());
        if (!view_bits || view_bits > 64) return false;
        low = view_bits == 64 ? literal.Payload()
                              : (literal.Payload() & ((u64{1} << view_bits) - 1));
    } else {
        return false;
    }
    const u64 exp_mask = bits == 64 ? (u64{0x7ff} << 52) : (u64{0xff} << 23);
    const u64 frac_mask = bits == 64 ? ((u64{1} << 52) - 1) : ((u64{1} << 23) - 1);
    for (unsigned i = 0; i < count; ++i) {
        const u64 word = bits == 64 ? (i == 0 ? low : high)
                                    : (((i < 2 ? low : high) >> ((i & 1) * 32)) & UINT32_MAX);
        if ((word & exp_mask) == 0 && (word & frac_mask) != 0) return false;
    }
    return true;
}

void Writer::RecordDenormalLanes(std::span<const std::uint8_t> inputs,
                                 unsigned bits,
                                 unsigned count,
                                 std::span<const std::uint8_t> peers,
                                 unsigned denominator,
                                 unsigned fma_flags,
                                 bool negative_invalid) {
    const unsigned fraction = bits == 32 ? 23 : 52, exponent = bits == 32 ? 8 : 11;
    const u64 exponent_mask = ((u64{1} << exponent) - 1) << fraction;
    const bool fused = fma_flags != UINT32_MAX;
    for (unsigned lane = 0; lane < count; ++lane) {
        std::array<u32, 16> done{};
        unsigned done_count{};
        const auto skip_zero = [&](const v::Register& reg) {
            done[done_count++] = masm.Here();
            __ cbz(reg, std::int64_t{0});
        };
        const auto skip_nonzero = [&](const v::Register& reg) {
            done[done_count++] = masm.Here();
            __ cbnz(reg, std::int64_t{0});
        };
        const auto read = [&](unsigned reg) { __ umov(R(16, bits), Lanes(reg, bits), lane); };
        const auto zero = [&](unsigned reg) {
            read(reg);
            __ lsl(R(17, bits), R(16, bits), 1);
            skip_zero(R(17, bits));
        };
        const auto nan = [&](unsigned reg) {
            read(reg);
            Extract(17, 16, fraction, exponent);
            __ eor(v::x17, v::x17, Operand((u64{1} << exponent) - 1));
            const auto finite = masm.Here();
            __ cbnz(v::x17, std::int64_t{0});
            __ lsl(R(17, bits), R(16, bits), bits - fraction);
            skip_nonzero(R(17, bits));
            masm.Patch(finite, masm.Here());
        };
        if (inputs.size() > 1) {
            std::array<u32, 3> denormal{};
            unsigned denormal_count{};
            for (const auto input : inputs) {
                read(input);
                Extract(17, 16, fraction, exponent);
                const auto normal_input = masm.Here();
                __ cbnz(v::x17, std::int64_t{0});
                __ lsl(R(17, bits), R(16, bits), 1);
                denormal[denormal_count++] = masm.Here();
                __ cbnz(R(17, bits), std::int64_t{0});
                masm.Patch(normal_input, masm.Here());
            }
            done[done_count++] = masm.Here();
            __ b(std::int64_t{0});
            for (unsigned i = 0; i < denormal_count; ++i) masm.Patch(denormal[i], masm.Here());
        } else {
            read(inputs.front());
            Extract(17, 16, fraction, exponent);
            skip_nonzero(v::x17);
            __ lsl(R(17, bits), R(16, bits), 1);
            skip_zero(R(17, bits));
        }
        if (negative_invalid) {
            done[done_count++] = masm.Here();
            __ tbnz(R(16, bits), bits - 1, std::int64_t{0});  // Negative sqrt: IE takes priority.
        }
        for (const auto peer : peers) nan(peer);
        if (denominator != UINT32_MAX) zero(denominator);  // ZE takes priority over same-lane DE.
        if (fused) {
            const auto a = peers[0], b = peers[1], c = peers[2];
            const auto infinity = [&](unsigned reg) {
                read(reg);
                __ and_(R(17, bits), R(16, bits), Operand((u64{1} << (bits - 1)) - 1));
                __ eor(R(17, bits), R(17, bits), Operand(exponent_mask));
                const auto site = masm.Here();
                __ cbz(R(17, bits), std::int64_t{0});
                return site;
            };
            const auto a_infinite = infinity(a), b_infinite = infinity(b);
            const auto finite_product = masm.Here();
            __ b(std::int64_t{0});
            masm.Patch(a_infinite, masm.Here());
            zero(b);
            const auto product_infinite = masm.Here();
            __ b(std::int64_t{0});
            masm.Patch(b_infinite, masm.Here());
            zero(a);
            masm.Patch(product_infinite, masm.Here());
            read(c);
            __ and_(R(17, bits), R(16, bits), Operand((u64{1} << (bits - 1)) - 1));
            __ eor(R(17, bits), R(17, bits), Operand(exponent_mask));
            const auto finite_addend = masm.Here();
            __ cbnz(R(17, bits), std::int64_t{0});
            read(a);
            __ umov(R(17, bits), Lanes(b, bits), lane);
            __ eor(R(16, bits), R(16, bits), Operand(R(17, bits)));
            __ umov(R(17, bits), Lanes(c, bits), lane);
            __ eor(R(16, bits), R(16, bits), Operand(R(17, bits)));
            const bool negate_c = bool(fma_flags & 2) ^
                                  (bool(fma_flags & 8) && ((lane & 1) == ((fma_flags >> 4) & 1)));
            if (bool(fma_flags & 1) != negate_c)
                __ eor(R(16, bits), R(16, bits), Operand(u64{1} << (bits - 1)));
            done[done_count++] = masm.Here();
            __ tbnz(R(16, bits), bits - 1, std::int64_t{0});
            masm.Patch(finite_addend, masm.Here());
            masm.Patch(finite_product, masm.Here());
        }
        __ mrs(v::x17, v::FPSR);
        __ orr(v::x17, v::x17, Operand(u64{1} << 7));
        __ msr(v::FPSR, v::x17);
        for (unsigned i = 0; i < done_count; ++i) masm.Patch(done[i], masm.Here());
    }
}

bool Writer::CanBatchDenormal(unsigned bits, unsigned count) const {
    if (count <= 1 || (bits != 32 && bits != 64) || long_forward_branches) return false;
    if (options.mxcsr_flush_mode != UINT8_MAX && (options.mxcsr_flush_mode & 1)) return false;
    return options.mxcsr_flush_offset == UINT32_MAX || options.mxcsr_flush_mode == 0;
}

bool Writer::HasUpcomingBatchOp(u32 from) const {
    if (!node || block.value >= source.Blocks().size()) return false;
    const auto& body = source.Blocks()[block.value];
    const u32 limit = std::min(body.first_node + body.node_count, from + 24u);
    const auto allow_bits = [&](u64 bits) {
        if ((bits == 64 && pending_denormal.check_mask_64) ||
            (bits == 32 && pending_denormal.check_mask_32))
            return true;
        return pending_denormal.preserved_count < 2;
    };
    for (u32 i = from; i < limit; ++i) {
        const auto& op = source.Operations()[i];
        if ((op.opcode == OpCode::Nop || op.opcode == OpCode::FaultLocation) &&
            op.Metadata() == OperationMetadata{})
            continue;
        if (op.opcode == OpCode::VecFAdd || op.opcode == OpCode::VecFSub ||
            op.opcode == OpCode::VecFMul || op.opcode == OpCode::VecFDiv) {
            const auto args = source.Operands(NodeId{i});
            auto at = args.begin();
            for (unsigned k = 0; k < 2 && at != args.end(); ++k) ++at;
            if (at == args.end() || (*at).Kind() != OperandKind::Immediate) return false;
            return allow_bits((*at).Payload());
        }
        if (op.opcode == OpCode::VecFCmpMask || op.opcode == OpCode::VecFMinMax) {
            const auto args = source.Operands(NodeId{i});
            auto at = args.begin();
            for (unsigned k = 0; k < 2 && at != args.end(); ++k) ++at;
            if (at == args.end() || (*at).Kind() != OperandKind::Immediate) return false;
            const u64 bits = (*at).Payload();
            CellView last{};
            for (const auto cell : args) last = cell;
            return last.Kind() == OperandKind::Immediate && last.Payload() == 0 &&
                   allow_bits(bits);
        }
        const auto effects = EffectsFor(op.opcode, op.Metadata());
        if (HasAnyEffect(effects,
                         Effect::MayFault | Effect::Unknown | Effect::External | Effect::Control |
                                 Effect::ReadHostState | Effect::WriteHostState))
            return false;
        if ((op.opcode == OpCode::LoadUniform || op.opcode == OpCode::StoreUniform ||
             op.opcode == OpCode::StoreUniformSelectImm) &&
            options.fpsr_offset != UINT32_MAX) {
            const auto args = source.Operands(NodeId{i});
            if (args.begin() != args.end() && (*args.begin()).Payload() == options.fpsr_offset)
                return false;
        }
    }
    return false;
}

bool Writer::PreserveBatchedRegister(unsigned reg) {
    if (!pending_denormal.op_count || !((pending_denormal.protected_mask >> reg) & 1u)) return true;
    const u32 pool = VectorPinned() ? 0x0000fcfcu : 0x3fff00fcu;
    const u32 avail = pool & ~allocation.used_vector_mask & ~pending_denormal.protected_mask;
    if (!avail) return false;
    const unsigned copy = std::countr_zero(avail);
    VectorMove(copy, reg);
    ++pending_denormal.preserved_count;
    const u32 old_bit = u32{1} << reg;
    const u32 new_bit = u32{1} << copy;
    if (pending_denormal.check_mask_32 & old_bit)
        pending_denormal.check_mask_32 = (pending_denormal.check_mask_32 & ~old_bit) | new_bit;
    if (pending_denormal.check_mask_64 & old_bit)
        pending_denormal.check_mask_64 = (pending_denormal.check_mask_64 & ~old_bit) | new_bit;
    pending_denormal.protected_mask = (pending_denormal.protected_mask & ~old_bit) | new_bit;
    for (unsigned k = 0; k < pending_denormal.op_count; ++k) {
        auto& op = pending_denormal.ops[k];
        for (unsigned i = 0; i < op.input_count; ++i)
            if (op.inputs[i] == reg) op.inputs[i] = static_cast<std::uint8_t>(copy);
        for (unsigned i = 0; i < op.peer_count; ++i)
            if (op.peers[i] == reg) op.peers[i] = static_cast<std::uint8_t>(copy);
        if (op.denominator == reg) op.denominator = static_cast<std::uint8_t>(copy);
    }
    return true;
}

u32 Writer::EmitBatchZeroFilter(u32 check_mask_32, u32 check_mask_64) {
    std::array<std::uint8_t, 32> regs64, regs32;
    unsigned reg_count_64{}, reg_count_32{};
    for (u32 mask = check_mask_64; mask; mask &= mask - 1)
        regs64[reg_count_64++] = static_cast<std::uint8_t>(std::countr_zero(mask));
    for (u32 mask = check_mask_32; mask; mask &= mask - 1)
        regs32[reg_count_32++] = static_cast<std::uint8_t>(std::countr_zero(mask));
    if (!reg_count_64 && !reg_count_32) return UINT32_MAX;
    const unsigned ones = 0u, t0 = 1u;
    const unsigned first = VectorTemporary(), second = VectorTemporary(true);
    __ cmeq(Lanes(ones, 64), Lanes(ones, 64), Lanes(ones, 64));
    if (reg_count_64) {
        __ shl(Lanes(first, 64), Lanes(regs64[0], 64), 1);
        __ add(Lanes(first, 64), Lanes(first, 64), Lanes(ones, 64));
        if (reg_count_64 > 1) {
            __ shl(Lanes(t0, 64), Lanes(regs64[1], 64), 1);
            __ add(Lanes(t0, 64), Lanes(t0, 64), Lanes(ones, 64));
            __ uzp2(Lanes(first, 32), Lanes(first, 32), Lanes(t0, 32));
        } else {
            __ uzp2(Lanes(first, 32), Lanes(first, 32), Lanes(first, 32));
        }
        for (unsigned i = 2; i < reg_count_64; i += 2) {
            __ shl(Lanes(second, 64), Lanes(regs64[i], 64), 1);
            __ add(Lanes(second, 64), Lanes(second, 64), Lanes(ones, 64));
            if (i + 1 < reg_count_64) {
                __ shl(Lanes(t0, 64), Lanes(regs64[i + 1], 64), 1);
                __ add(Lanes(t0, 64), Lanes(t0, 64), Lanes(ones, 64));
                __ uzp2(Lanes(second, 32), Lanes(second, 32), Lanes(t0, 32));
            } else {
                __ uzp2(Lanes(second, 32), Lanes(second, 32), Lanes(second, 32));
            }
            __ umin(Lanes(first, 32), Lanes(first, 32), Lanes(second, 32));
        }
    }
    if (reg_count_32) {
        const unsigned acc32 = reg_count_64 ? second : first;
        __ shl(Lanes(acc32, 32), Lanes(regs32[0], 32), 1);
        __ add(Lanes(acc32, 32), Lanes(acc32, 32), Lanes(ones, 32));
        for (unsigned i = 1; i < reg_count_32; ++i) {
            __ shl(Lanes(t0, 32), Lanes(regs32[i], 32), 1);
            __ add(Lanes(t0, 32), Lanes(t0, 32), Lanes(ones, 32));
            __ umin(Lanes(acc32, 32), Lanes(acc32, 32), Lanes(t0, 32));
        }
        if (reg_count_64) {
            __ ushr(Lanes(acc32, 32), Lanes(acc32, 32), 3);
            __ umin(Lanes(first, 32), Lanes(first, 32), Lanes(acc32, 32));
        }
    }
    __ uminp(Lanes(first, 32), Lanes(first, 32), Lanes(first, 32));
    __ uminp(Lanes(first, 32, 64), Lanes(first, 32, 64), Lanes(first, 32, 64));
    __ fmov(v::w16, S(first));
    __ lsr(v::w16, v::w16, reg_count_64 ? 21 : 24);
    const u32 zero_only = masm.Here();
    __ cbnz(v::w16, std::int64_t{0});
    return zero_only;
}

void Writer::FlushDenormalBatch() {
    if (!pending_denormal.op_count) return;
    const u32 check_mask_32 = pending_denormal.check_mask_32;
    const u32 check_mask_64 = pending_denormal.check_mask_64;
    const unsigned op_count = pending_denormal.op_count;
    std::array<DenormalBatchOp, 16> ops;
    for (unsigned k = 0; k < op_count; ++k) ops[k] = pending_denormal.ops[k];
    pending_denormal.check_mask_32 = 0;
    pending_denormal.check_mask_64 = 0;
    pending_denormal.protected_mask = 0;
    pending_denormal.op_count = 0;
    pending_denormal.preserved_count = 0;
    if (op_count == 1) {
        const auto& op = ops[0];
        const u32 denom = op.denominator == UINT8_MAX ? UINT32_MAX : u32{op.denominator};
        if (op.input_count == 1) {
            if (op.peer_count == 1)
                RecordDenormalInput(op.inputs[0], op.bits, op.count, {op.peers[0]}, denom);
            else
                RecordDenormalInput(op.inputs[0], op.bits, op.count, {}, denom);
        } else if (op.input_count == 2) {
            RecordDenormalInputs(op.inputs[0], op.inputs[1], op.bits, op.count, denom);
        }
        return;
    }
    std::array<std::uint8_t, 32> regs64, regs32;
    unsigned reg_count_64{}, reg_count_32{};
    for (u32 mask = check_mask_64; mask; mask &= mask - 1)
        regs64[reg_count_64++] = static_cast<std::uint8_t>(std::countr_zero(mask));
    for (u32 mask = check_mask_32; mask; mask &= mask - 1)
        regs32[reg_count_32++] = static_cast<std::uint8_t>(std::countr_zero(mask));
    if (!reg_count_64 && !reg_count_32) return;
    const unsigned t0 = 0u, t1 = 1u;
    const unsigned first = VectorTemporary(), second = VectorTemporary(true);
    const auto emit_pair64 = [&](unsigned dst, unsigned idx) {
        const unsigned r0 = regs64[idx];
        const unsigned r1 = idx + 1 < reg_count_64 ? regs64[idx + 1] : r0;
        __ uzp2(Lanes(dst, 32), Lanes(r0, 32), Lanes(r1, 32));
        __ shl(Lanes(dst, 32), Lanes(dst, 32), 1);
    };
    if (reg_count_64) {
        emit_pair64(first, 0);
        if (reg_count_64 > 2) {
            emit_pair64(second, 2);
            unsigned pair_idx = 0;
            for (unsigned i = 4; i < reg_count_64; i += 2, ++pair_idx) {
                const unsigned tmp = (pair_idx & 1u) ? t1 : t0;
                const unsigned acc = (pair_idx & 1u) ? second : first;
                emit_pair64(tmp, i);
                __ umin(Lanes(acc, 32), Lanes(acc, 32), Lanes(tmp, 32));
            }
            __ umin(Lanes(first, 32), Lanes(first, 32), Lanes(second, 32));
        }
    }
    if (reg_count_32) {
        if (!reg_count_64) {
            __ shl(Lanes(first, 32), Lanes(regs32[0], 32), 1);
            if (reg_count_32 > 1) {
                __ shl(Lanes(second, 32), Lanes(regs32[1], 32), 1);
                for (unsigned i = 2; i < reg_count_32; ++i) {
                    const unsigned tmp = (i & 1u) ? t1 : t0;
                    const unsigned acc = (i & 1u) ? second : first;
                    __ shl(Lanes(tmp, 32), Lanes(regs32[i], 32), 1);
                    __ umin(Lanes(acc, 32), Lanes(acc, 32), Lanes(tmp, 32));
                }
                __ umin(Lanes(first, 32), Lanes(first, 32), Lanes(second, 32));
            }
        } else {
            __ shl(Lanes(second, 32), Lanes(regs32[0], 32), 1);
            if (reg_count_32 > 1) {
                __ shl(Lanes(t1, 32), Lanes(regs32[1], 32), 1);
                for (unsigned i = 2; i < reg_count_32; ++i) {
                    const unsigned acc = (i & 1u) ? t1 : second;
                    __ shl(Lanes(t0, 32), Lanes(regs32[i], 32), 1);
                    __ umin(Lanes(acc, 32), Lanes(acc, 32), Lanes(t0, 32));
                }
                __ umin(Lanes(second, 32), Lanes(second, 32), Lanes(t1, 32));
            }
            __ ushr(Lanes(second, 32), Lanes(second, 32), 3);
            __ umin(Lanes(first, 32), Lanes(first, 32), Lanes(second, 32));
        }
    }
    __ uminv(S(first), Lanes(first, 32));
    __ fmov(v::w16, S(first));
    __ lsr(v::w16, v::w16, reg_count_64 ? 21 : 24);
    if (!long_forward_branches && cold_denormal_stubs.size() < max_cold_denormal_stubs) {
        if (cold_denormal_stubs.empty()) cold_denormal_stubs.reserve(16);
        const auto branch = masm.Here();
        __ cbz(v::w16, std::int64_t{0});
        cold_denormal_stubs.emplace_back();
        auto& stub = cold_denormal_stubs.back();
        stub.branch_words[0] = branch;
        stub.branch_count = 1;
        stub.return_word = masm.Here();
        stub.bits = 0;
        stub.count = 0;
        stub.batch_op_count = static_cast<std::uint8_t>(op_count);
        stub.check_mask_32 = check_mask_32;
        stub.check_mask_64 = check_mask_64;
        for (unsigned k = 0; k < op_count; ++k) stub.batch_ops[k] = ops[k];
        return;
    }
    const auto disabled = DenormalControlGuard();
    const auto normal = masm.Here();
    __ cbnz(v::x16, std::int64_t{0});
    for (unsigned k = 0; k < op_count; ++k) {
        const auto& op = ops[k];
        const u32 denom = op.denominator == UINT8_MAX ? UINT32_MAX : u32{op.denominator};
        RecordDenormalLanes(std::span<const std::uint8_t>{op.inputs.data(), op.input_count},
                            op.bits,
                            op.count,
                            std::span<const std::uint8_t>{op.peers.data(), op.peer_count},
                            denom,
                            UINT32_MAX,
                            false);
    }
    masm.Patch(normal, masm.Here());
    if (disabled != UINT32_MAX) masm.Patch(disabled, masm.Here());
}

void Writer::QueueDenormalInputs(unsigned left,
                                 unsigned right,
                                 unsigned bits,
                                 unsigned count,
                                 unsigned denominator,
                                 bool skip_left,
                                 bool skip_right,
                                 u32 clobber_mask) {
    if (options.mxcsr_flush_mode != UINT8_MAX && (options.mxcsr_flush_mode & 1)) return;
    if (skip_left && skip_right) {
        if (pending_denormal.op_count && (pending_denormal.protected_mask & clobber_mask)) {
            bool preserved = HasUpcomingBatchOp(node.value + 1);
            for (u32 m = pending_denormal.protected_mask & clobber_mask; preserved && m;
                 m &= m - 1) {
                preserved = PreserveBatchedRegister(std::countr_zero(m));
            }
            if (!preserved) FlushDenormalBatch();
        }
        return;
    }
    const bool check_left = !skip_left && IsStableVectorHome(left);
    const bool check_right = !skip_right && IsStableVectorHome(right);
    const bool peer_left = IsStableVectorHome(left);
    const bool peer_right = IsStableVectorHome(right);
    const bool denom_ok = denominator == UINT32_MAX || IsStableVectorHome(denominator);
    if (!CanBatchDenormal(bits, count) || (!skip_left && !check_left) ||
        (!skip_right && !check_right) || !peer_left || !peer_right || !denom_ok) {
        FlushDenormalBatch();
        RecordDenormalInputs(left, right, bits, count, denominator, skip_left, skip_right);
        return;
    }
    if (pending_denormal.op_count == pending_denormal.ops.size()) {
        FlushDenormalBatch();
    }
    if (cold_denormal_stubs.size() >= max_cold_denormal_stubs) {
        FlushDenormalBatch();
        RecordDenormalInputs(left, right, bits, count, denominator, skip_left, skip_right);
        return;
    }
    auto& op = pending_denormal.ops[pending_denormal.op_count++];
    op.denominator = denominator == UINT32_MAX ? UINT8_MAX : static_cast<std::uint8_t>(denominator);
    op.input_count = 0;
    op.peer_count = 0;
    op.bits = static_cast<std::uint8_t>(bits);
    op.count = static_cast<std::uint8_t>(count);
    u32& check_mask = bits == 64 ? pending_denormal.check_mask_64 : pending_denormal.check_mask_32;
    if (!skip_left) {
        op.inputs[op.input_count++] = static_cast<std::uint8_t>(left);
        check_mask |= u32{1} << left;
    }
    if (!skip_right && (skip_left || right != left)) {
        op.inputs[op.input_count++] = static_cast<std::uint8_t>(right);
        check_mask |= u32{1} << right;
    }
    if (skip_left) {
        op.peers[op.peer_count++] = static_cast<std::uint8_t>(left);
    } else if (skip_right) {
        op.peers[op.peer_count++] = static_cast<std::uint8_t>(right);
    } else if (left != right) {
        op.peers[op.peer_count++] = static_cast<std::uint8_t>(left);
        op.peers[op.peer_count++] = static_cast<std::uint8_t>(right);
    }
    pending_denormal.protected_mask |= (u32{1} << left) | (u32{1} << right);
    if (denominator != UINT32_MAX) pending_denormal.protected_mask |= u32{1} << denominator;
    if (pending_denormal.op_count == pending_denormal.ops.size()) {
        FlushDenormalBatch();
        return;
    }
    if (const u32 conflict = pending_denormal.protected_mask & clobber_mask) {
        bool preserved = HasUpcomingBatchOp(node.value + 1);
        for (u32 m = conflict; preserved && m; m &= m - 1) {
            preserved = PreserveBatchedRegister(std::countr_zero(m));
        }
        if (!preserved) FlushDenormalBatch();
    }
}

void Writer::RecordDenormalInput(unsigned value,
                                 unsigned bits,
                                 unsigned count,
                                 std::initializer_list<unsigned> peers,
                                 unsigned denominator,
                                 unsigned fma_flags) {
    if (options.mxcsr_flush_mode != UINT8_MAX && (options.mxcsr_flush_mode & 1)) return;
    const bool fused = fma_flags != UINT32_MAX;
    std::array<std::uint8_t, 3> input_regs{};
    std::uint8_t input_count{};
    if (fused) {
        for (const auto peer : peers) input_regs[input_count++] = static_cast<std::uint8_t>(peer);
    } else {
        input_regs[input_count++] = static_cast<std::uint8_t>(value);
    }
    std::array<std::uint8_t, 3> peer_regs{};
    std::uint8_t peer_count{};
    for (const auto peer : peers) peer_regs[peer_count++] = static_cast<std::uint8_t>(peer);
    const bool negative_invalid =
            node && source.Operations()[node.value].opcode == OpCode::VecFUnary;
    const bool defer_cold =
            !long_forward_branches && cold_denormal_stubs.size() < max_cold_denormal_stubs;
    const u32 disabled = defer_cold ? UINT32_MAX : DenormalControlGuard();
    std::array<u32, 3> rare{};
    std::uint8_t rare_count{};
    if (count == 1) {
        for (unsigned i = 0; i < input_count; ++i) {
            const unsigned input = input_regs[i];
            __ fmov(R(16, bits), bits == 32 ? S(input) : D(input));
            __ lsl(R(16, bits), R(16, bits), 1);
            __ sub(R(16, bits), R(16, bits), Operand(1));
            __ lsr(R(16, bits), R(16, bits), bits == 32 ? 24 : 53);
            rare[rare_count++] = masm.Here();
            __ cbz(v::w16, std::int64_t{0});
        }
    } else {
        unsigned scratch{};
        for (const unsigned candidate : {VectorTemporary(), VectorTemporary(true), 0u, 1u}) {
            bool used = candidate == value;
            for (const auto peer : peers) used |= candidate == peer;
            if (!used) {
                scratch = candidate;
                break;
            }
        }
        for (unsigned i = 0; i < input_count; ++i) {
            const unsigned input = input_regs[i];
            if (bits == 64) {
                __ uzp2(Lanes(scratch, 32), Lanes(input, 32), Lanes(input, 32));
                __ shl(Lanes(scratch, 32), Lanes(scratch, 32), 1);
                __ uminp(Lanes(scratch, 32, 64), Lanes(scratch, 32, 64), Lanes(scratch, 32, 64));
                __ fmov(v::w16, S(scratch));
                __ lsr(v::w16, v::w16, 21);
            } else {
                __ shl(Lanes(scratch, 32), Lanes(input, 32), 1);
                __ uminv(S(scratch), Lanes(scratch, 32));
                __ fmov(v::w16, S(scratch));
                __ lsr(v::w16, v::w16, 24);
            }
            rare[rare_count++] = masm.Here();
            __ cbz(v::w16, std::int64_t{0});
        }
    }
    if (defer_cold) {
        if (cold_denormal_stubs.empty()) cold_denormal_stubs.reserve(16);
        cold_denormal_stubs.emplace_back();
        auto& stub = cold_denormal_stubs.back();
        stub.branch_words = rare;
        stub.return_word = masm.Here();
        stub.denominator = denominator;
        stub.fma_flags = fma_flags;
        stub.inputs = input_regs;
        stub.peers = peer_regs;
        stub.branch_count = rare_count;
        stub.input_count = input_count;
        stub.peer_count = peer_count;
        stub.bits = static_cast<std::uint8_t>(bits);
        stub.count = static_cast<std::uint8_t>(count);
        stub.negative_invalid = negative_invalid;
        stub.batch_op_count = 0;
        return;
    }
    const auto normal = masm.Here();
    __ b(std::int64_t{0});
    for (unsigned i = 0; i < rare_count; ++i) masm.Patch(rare[i], masm.Here());
    RecordDenormalLanes(std::span<const std::uint8_t>{input_regs.data(), input_count},
                        bits,
                        count,
                        std::span<const std::uint8_t>{peer_regs.data(), peer_count},
                        denominator,
                        fma_flags,
                        negative_invalid);
    masm.Patch(normal, masm.Here());
    if (disabled != UINT32_MAX) masm.Patch(disabled, masm.Here());
}

void Writer::RecordDenormalInputs(unsigned left,
                                  unsigned right,
                                  unsigned bits,
                                  unsigned count,
                                  unsigned denominator,
                                  bool skip_left,
                                  bool skip_right) {
    if (skip_left && skip_right) return;
    if (skip_left) {
        RecordDenormalInput(right, bits, count, {left}, denominator);
        return;
    }
    if (skip_right || left == right) {
        if (left == right)
            RecordDenormalInput(left, bits, count, {}, denominator);
        else
            RecordDenormalInput(left, bits, count, {right}, denominator);
        return;
    }
    if (count == 1) {
        RecordDenormalInput(left, bits, count, {right}, denominator);
        RecordDenormalInput(right, bits, count, {left}, denominator);
        return;
    }
    if (options.mxcsr_flush_mode != UINT8_MAX && (options.mxcsr_flush_mode & 1)) return;
    const bool defer_cold =
            !long_forward_branches && cold_denormal_stubs.size() < max_cold_denormal_stubs;
    const auto disabled = defer_cold ? UINT32_MAX : DenormalControlGuard();
    // All candidates are reserved scratches, never pinned/allocated homes.
    // Spilled inputs can occupy v0/v1, so exclude both actual source registers.
    std::array<unsigned, 2> scratch{};
    unsigned available{};
    for (const auto candidate : {VectorTemporary(), VectorTemporary(true), 0u, 1u}) {
        if (candidate == left || candidate == right) continue;
        scratch[available++] = candidate;
        if (available == scratch.size()) break;
    }
    const auto first = scratch[0], second = scratch[1];
    if (bits == 64) {
        __ uzp2(Lanes(first, 32), Lanes(left, 32), Lanes(right, 32));
        __ shl(Lanes(first, 32), Lanes(first, 32), 1);
    } else {
        __ shl(Lanes(first, 32), Lanes(left, 32), 1);
        __ shl(Lanes(second, 32), Lanes(right, 32), 1);
        __ umin(Lanes(first, 32), Lanes(first, 32), Lanes(second, 32));
    }
    __ uminv(S(first), Lanes(first, 32));
    __ fmov(v::w16, S(first));
    __ lsr(v::w16, v::w16, bits == 32 ? 24 : 21);
    const std::array<std::uint8_t, 3> inputs{
            static_cast<std::uint8_t>(left), static_cast<std::uint8_t>(right), 0};
    if (defer_cold) {
        if (cold_denormal_stubs.empty()) cold_denormal_stubs.reserve(16);
        const auto branch = masm.Here();
        __ cbz(v::w16, std::int64_t{0});
        cold_denormal_stubs.emplace_back();
        auto& stub = cold_denormal_stubs.back();
        stub.branch_words = {branch, 0, 0};
        stub.return_word = masm.Here();
        stub.denominator = denominator;
        stub.fma_flags = UINT32_MAX;
        stub.inputs = inputs;
        stub.peers = inputs;
        stub.branch_count = 1;
        stub.input_count = 2;
        stub.peer_count = 2;
        stub.bits = static_cast<std::uint8_t>(bits);
        stub.count = static_cast<std::uint8_t>(count);
        stub.negative_invalid = false;
        stub.batch_op_count = 0;
        if (IsStableVectorHome(left) && IsStableVectorHome(right)) {
            const u32 pair_mask = (u32{1} << left) | (u32{1} << right);
            if (bits == 64)
                stub.check_mask_64 = pair_mask;
            else
                stub.check_mask_32 = pair_mask;
        }
        return;
    }
    const auto normal = masm.Here();
    __ cbnz(v::x16, std::int64_t{0});
    // One lane classification handles both inputs and checks NaN/invalid
    // precedence before setting DE. No source or guest integer flags change.
    RecordDenormalLanes(std::span<const std::uint8_t>{inputs.data(), 2},
                        bits,
                        count,
                        std::span<const std::uint8_t>{inputs.data(), 2},
                        denominator,
                        UINT32_MAX,
                        false);
    masm.Patch(normal, masm.Here());
    if (disabled != UINT32_MAX) masm.Patch(disabled, masm.Here());
}

void Writer::RecordNaNInput(unsigned value, unsigned bits, unsigned count, bool signaling) {
    const unsigned fraction = bits == 32 ? 23 : 52, exponent = bits == 32 ? 8 : 11;
    for (unsigned lane = 0; lane < count; ++lane) {
        __ umov(R(16, bits), Lanes(value, bits), lane);
        Extract(17, 16, fraction, exponent);
        __ eor(v::x17, v::x17, Operand((u64{1} << exponent) - 1));
        const auto finite = masm.Here();
        __ cbnz(v::x17, std::int64_t{0});
        __ lsl(R(17, bits), R(16, bits), bits - fraction);
        const auto infinity = masm.Here();
        __ cbz(R(17, bits), std::int64_t{0});
        u32 quiet = UINT32_MAX;
        if (!signaling) {
            quiet = masm.Here();
            __ tbnz(R(16, bits), fraction - 1, std::int64_t{0});
        }
        __ mrs(v::x17, v::FPSR);
        __ orr(v::x17, v::x17, Operand(u64{1}));
        __ msr(v::FPSR, v::x17);
        masm.Patch(finite, masm.Here());
        masm.Patch(infinity, masm.Here());
        if (quiet != UINT32_MAX) masm.Patch(quiet, masm.Here());
    }
}

}  // namespace swift::runtime::backend::arm64::detail

#undef __
