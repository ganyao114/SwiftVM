#include <algorithm>
#include "emit.h"

namespace swift::runtime::backend::x64::detail {
ir::Type Writer::View(CellView c) const {
    return c.TypeView() != ir::Type::Void   ? c.TypeView()
           : c.Kind() == OperandKind::Value ? source.Operations()[c.Payload()].type
                                            : ir::Type::U64;
}

bool Writer::Immediate32(CellView c, unsigned width, std::int32_t& immediate) const {
    if (c.Kind() != OperandKind::Immediate || (width != 32 && width != 64)) return false;
    auto value = c.Payload();
    const auto bits = TypeBits(View(c));
    if (bits && bits < 64) value &= (u64{1} << bits) - 1;
    immediate = static_cast<std::int32_t>(value);
    // A 64-bit x64 instruction sign-extends its imm32. U32 0xffffffff is
    // positive when read through that view and must keep the register form.
    return width == 32 || value == u64(std::int64_t(immediate));
}

bool Writer::ResidentScalar(CellView value, unsigned width, Reg64& result) const {
    if (width < 32 || in_call || value.Kind() != OperandKind::Value) return false;
    const auto natural = source.Operations()[value.Payload()].type;
    const auto& home = allocation.homes[value.Payload()];
    if (natural == ir::Type::V128 || home.spilled ||
        (TypeBits(View(value)) < width && natural != View(value)))
        return false;
    result = Reg64(home.reg);
    return true;
}

void Writer::Mask(const Reg64& r, unsigned bits) {
    if (bits == 32)
        mov(r.cvt32(), r.cvt32());
    else if (bits && bits < 32)
        and_(r.cvt32(), (u32{1} << bits) - 1);
}

void Writer::Sign(const Reg64& r, unsigned bits) {
    if (bits && bits < 64) {
        shl(r, 64 - bits);
        sar(r, 64 - bits);
    }
}

void Writer::Load(const Reg64& r, const Reg64& b, std::int32_t off, unsigned bits) {
    if (bits <= 8)
        movzx(r.cvt32(), byte[b + off]);
    else if (bits == 16)
        movzx(r.cvt32(), word[b + off]);
    else if (bits == 32)
        mov(r.cvt32(), dword[b + off]);
    else if (bits == 64)
        mov(r, qword[b + off]);
    else
        Reject(CompileError::UnsupportedType);
}

void Writer::Store(const Reg64& r, const Reg64& b, std::int32_t off, unsigned bits) {
    if (bits <= 8)
        mov(byte[b + off], r.cvt8());
    else if (bits == 16)
        mov(word[b + off], r.cvt16());
    else if (bits == 32)
        mov(dword[b + off], r.cvt32());
    else if (bits == 64)
        mov(qword[b + off], r);
    else
        Reject(CompileError::UnsupportedType);
}

void Writer::Read(CellView c, const Reg64& r, bool raw) {
    const bool narrow = !raw && TypeBits(View(c)) == 32;
    if (c.Kind() == OperandKind::Value) {
        const auto& h = allocation.homes[c.Payload()];
        if (h.spilled && h.uniform_offset != UINT32_MAX) {
            const auto natural_bits = std::min(64u, TypeBits(source.Operations()[c.Payload()].type));
            const auto view_bits = std::min(64u, TypeBits(View(c)));
            const auto load_bits = raw ? natural_bits : std::min(natural_bits, view_bits);
            Load(r, r12, h.uniform_offset, load_bits >= 8 ? load_bits : 8);
            if (!raw && load_bits < 8) Mask(r, load_bits);
            return;
        }
        if (source.Operations()[c.Payload()].type == ir::Type::V128) {
            if (h.spilled) {
                if (narrow)
                    mov(r.cvt32(), dword[rsp + h.stack_offset]);
                else
                    mov(r, qword[rsp + h.stack_offset]);
            } else if (narrow) {
                if (options.avx)
                    vmovd(r.cvt32(), Xmm(h.reg));
                else
                    movd(r.cvt32(), Xmm(h.reg));
            } else {
                if (options.avx)
                    vmovq(r, Xmm(h.reg));
                else
                    movq(r, Xmm(h.reg));
            }
        } else {
            if (h.rematerialize) {
                u64 imm = source.Operands(NodeId{u32(c.Payload())}).front().Payload();
                const auto nbits = TypeBits(source.Operations()[c.Payload()].type);
                if (nbits && nbits < 64) imm &= (u64{1} << nbits) - 1;
                if (narrow)
                    mov(r.cvt32(), u32(imm));
                else
                    mov(r, imm);
            } else if (h.spilled || (in_call && h.reg >= 6 && h.reg <= 9)) {
                const auto offset =
                        h.spilled ? h.stack_offset : allocation.call_offset + 192 + (h.reg - 6) * 8;
                if (narrow)
                    mov(r.cvt32(), dword[rsp + offset]);
                else
                    mov(r, qword[rsp + offset]);
            } else if (narrow) {
                // U32 homes are normalized when their result is written.
                // A reused home already has the required upper zero bits.
                if (r.getIdx() != h.reg || source.Operations()[c.Payload()].type != ir::Type::U32)
                    mov(r.cvt32(), Reg32(h.reg));
            } else if (r.getIdx() != h.reg)
                mov(r, Reg64(h.reg));
        }
    } else if (c.Kind() == OperandKind::Immediate || c.Kind() == OperandKind::Condition ||
               c.Kind() == OperandKind::Flags || c.Kind() == OperandKind::Lambda) {
        if (narrow)
            mov(r.cvt32(), u32(c.Payload()));
        else
            mov(r, c.Payload());
    } else
        Reject(CompileError::UnsupportedOperand);
    if (!raw && !narrow) Mask(r, std::min(64u, TypeBits(View(c))));
}

void Writer::ReadVector(CellView c, const Xmm& r) {
    if (c.Kind() == OperandKind::Value) {
        const auto natural = source.Operations()[c.Payload()].type;
        const auto& h = allocation.homes[c.Payload()];
        if (natural == ir::Type::V128) {
            if (r.getIdx() == 0 && xmm0_node == c.Payload()) return;
            if (h.spilled && h.uniform_offset != UINT32_MAX) {
                if (options.avx)
                    vmovdqu(r, ptr[r12 + h.uniform_offset]);
                else
                    movdqu(r, ptr[r12 + h.uniform_offset]);
            } else if (h.spilled) {
                if (options.avx)
                    vmovdqu(r, ptr[rsp + h.stack_offset]);
                else
                    movdqu(r, ptr[rsp + h.stack_offset]);
            } else if (r.getIdx() != h.reg) {
                if (options.avx)
                    vmovdqa(r, Xmm(h.reg));
                else
                    movdqa(r, Xmm(h.reg));
            }
            if (r.getIdx() == 0) xmm0_node = static_cast<u32>(c.Payload());
            return;
        }
        if (r.getIdx() == 0) xmm0_node = NodeId::invalid;
        if (!h.rematerialize && (natural == ir::Type::U64 || natural == ir::Type::F64)) {
            if (h.spilled && h.uniform_offset != UINT32_MAX) {
                if (options.avx)
                    vmovq(r, qword[r12 + h.uniform_offset]);
                else
                    movq(r, qword[r12 + h.uniform_offset]);
            } else if (h.spilled || (in_call && h.reg >= 6 && h.reg <= 9)) {
                const auto off =
                        h.spilled ? h.stack_offset : allocation.call_offset + 192 + (h.reg - 6) * 8;
                if (options.avx)
                    vmovq(r, qword[rsp + off]);
                else
                    movq(r, qword[rsp + off]);
            } else if (options.avx)
                vmovq(r, Reg64(h.reg));
            else
                movq(r, Reg64(h.reg));
            return;
        }
        if (!h.rematerialize && (natural == ir::Type::U32 || natural == ir::Type::F32)) {
            if (h.spilled && h.uniform_offset != UINT32_MAX) {
                if (options.avx)
                    vmovd(r, dword[r12 + h.uniform_offset]);
                else
                    movd(r, dword[r12 + h.uniform_offset]);
            } else if (h.spilled || (in_call && h.reg >= 6 && h.reg <= 9)) {
                const auto off =
                        h.spilled ? h.stack_offset : allocation.call_offset + 192 + (h.reg - 6) * 8;
                if (options.avx)
                    vmovd(r, dword[rsp + off]);
                else
                    movd(r, dword[rsp + off]);
            } else if (options.avx)
                vmovd(r, Reg32(h.reg));
            else
                movd(r, Reg32(h.reg));
            return;
        }
    }
    if (r.getIdx() == 0) xmm0_node = NodeId::invalid;
    Read(c, r10, true);
    if (options.avx)
        vmovq(r, r10);
    else
        movq(r, r10);
}

void Writer::Expression(CellRange::Iterator& at,
                        CellRange::Iterator end,
                        const Reg64& out,
                        unsigned depth) {
    if (at == end) Reject(CompileError::UnsupportedOperand);
    const auto c = *at++;
    if (c.Kind() != OperandKind::Expression) {
        Read(c, out);
        return;
    }
    Expression(at, end, out, depth);
    if (c.Count() == 1) return;
    constexpr std::array<unsigned, 4> temp{1, 2, 10, 11};
    while (depth < temp.size() && temp[depth] == unsigned(out.getIdx())) ++depth;
    if (depth >= temp.size()) Reject(CompileError::UnsupportedOperand);
    const Reg64 rhs(temp[depth]);
    Expression(at, end, rhs, depth + 1);
    switch (c.ExpressionOp()) {
        case OperandOp::Add:
            lea(out, ptr[out + rhs]);
            break;
        case OperandOp::Sub:
            sub(out, rhs);
            break;
        case OperandOp::AddShift:
            if (c.ShiftAmount() <= 3)
                lea(out, ptr[out + rhs * (1u << c.ShiftAmount())]);
            else {
                shl(rhs, c.ShiftAmount());
                add(out, rhs);
            }
            break;
        case OperandOp::Lsl:
        case OperandOp::Lsr:
        case OperandOp::Asr: {
            // CL is fixed by the ISA. Exchange with the consumed RHS so an
            // enclosing expression's RCX value remains live across this shift.
            const bool output_is_count = out.getIdx() == 1;
            if (rhs.getIdx() != 1) xchg(rcx, rhs);
            const Reg64 shifted = output_is_count ? rhs : out;
            if (c.ExpressionOp() == OperandOp::Lsl)
                shl(shifted, cl);
            else if (c.ExpressionOp() == OperandOp::Lsr)
                shr(shifted, cl);
            else
                sar(shifted, cl);
            if (output_is_count)
                mov(rcx, shifted);
            else if (rhs.getIdx() != 1)
                xchg(rcx, rhs);
            break;
        }
        default:
            Reject(CompileError::UnsupportedOperand);
    }
}

void Writer::Handle(Op<OpCode::LoadImm>, OpContext& c) {
    if (allocation.homes[c.node.value].rematerialize) {
        c.saved = true;
        return;
    }
    const auto payload = c.Arg(0).Payload();
    if (c.vector_result) {
        if (payload == 0) {
            if (options.avx)
                vpxor(xmm0, xmm0, xmm0);
            else
                pxor(xmm0, xmm0);
            return;
        }
        mov(rax, payload);
        if (options.avx)
            vmovq(xmm0, rax);
        else
            movq(xmm0, rax);
        return;
    }
    const auto destination = ScalarDestination(c);
    if (payload == 0 && !(options.local_flags && flags.dirty && (flags.native || flags.floating))) {
        xor_(destination.cvt32(), destination.cvt32());
        c.scalar_normalized = true;
    } else if (c.width == 32 || (c.width == 64 && payload <= UINT32_MAX)) {
        mov(destination.cvt32(), u32(payload));
        c.scalar_normalized = true;
    } else
        mov(destination, payload);
}

void Writer::Handle(Op<OpCode::Zero>, OpContext& c) {
    if (c.vector_result) {
        if (options.avx)
            vpxor(xmm0, xmm0, xmm0);
        else
            pxor(xmm0, xmm0);
    } else {
        const auto destination = ScalarDestination(c);
        xor_(destination.cvt32(), destination.cvt32());
        c.scalar_normalized = true;
    }
}

void Writer::Handle(Op<OpCode::BitCast>, OpContext& c) {
    if (c.vector_result) {
        const auto& home = allocation.homes[c.node.value];
        const Xmm dst = (!home.spilled && !home.terminal_only) ? Xmm(home.reg) : xmm0;
        c.vector = dst;
        const auto arg = c.Arg(0);
        if (arg.Kind() == OperandKind::Value) {
            const auto load_id = static_cast<u32>(arg.Payload());
            const auto& lop = source.Operations()[load_id];
            if (allocation.homes[load_id].rematerialize &&
                (lop.opcode == OpCode::LoadMemory || lop.opcode == OpCode::LoadMemoryTSO)) {
                const auto ops = source.Operands(NodeId{load_id});
                auto at = ops.begin();
                Reg64 base = r11;
                std::int32_t disp = 0;
                MemoryAddress(at, ops.end(), base, disp);
                if (dst.getIdx() == 0) xmm0_node = NodeId::invalid;
                const auto bits = TypeBits(lop.type);
                const auto first = static_cast<u32>(getSize());
                if (bits == 64) {
                    if (options.avx)
                        vmovq(dst, qword[base + disp]);
                    else
                        movq(dst, qword[base + disp]);
                } else {
                    if (options.avx)
                        vmovd(dst, dword[base + disp]);
                    else
                        movd(dst, dword[base + disp]);
                }
                RecordGuestAccess(first, 1, bits / 8);
                return;
            }
        }
        ReadVector(arg, dst);
    } else
        Read(c.Arg(0), ScalarDestination(c), true);
}

void Writer::Handle(Op<OpCode::GetResult>, OpContext& c) { Handle(Op<OpCode::BitCast>{}, c); }

void Writer::Handle(Op<OpCode::GetOperand>, OpContext& c) {
    auto a = c.operands.begin();
    Expression(a, c.operands.end(), rax);
}

void Writer::Handle(Op<OpCode::ZeroExtend32>, OpContext& c) {
    const auto destination = ScalarDestination(c);
    Read(c.Arg(0), destination);
    if (TypeBits(View(c.Arg(0))) > 32) Mask(destination, 32);
    c.scalar_normalized = true;
}

void Writer::Handle(Op<OpCode::ZeroExtend32To64>, OpContext& c) {
    Handle(Op<OpCode::ZeroExtend32>{}, c);
}

void Writer::Handle(Op<OpCode::ZeroExtend64>, OpContext& c) {
    const auto destination = ScalarDestination(c);
    Read(c.Arg(0), destination);
    c.scalar_normalized = true;
}

void Writer::Handle(Op<OpCode::SignExtend>, OpContext& c) {
    const auto destination = ScalarDestination(c);
    Read(c.Arg(0), destination);
    Sign(destination, TypeBits(View(c.Arg(0))));
    c.scalar_normalized = c.width == 64;
}
}  // namespace swift::runtime::backend::x64::detail
