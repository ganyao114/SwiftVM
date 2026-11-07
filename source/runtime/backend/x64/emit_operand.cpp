#include "emit.h"
#include <algorithm>

namespace swift::runtime::backend::x64::detail {
ir::Type Writer::View(CellView c) const {
    return c.TypeView() != ir::Type::Void   ? c.TypeView()
           : c.Kind() == OperandKind::Value ? source.Operations()[c.Payload()].type
                                            : ir::Type::U64;
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
    if (c.Kind() == OperandKind::Value) {
        const auto& h = allocation.homes[c.Payload()];
        if (source.Operations()[c.Payload()].type == ir::Type::V128) {
            if (h.spilled)
                mov(r, qword[rsp + h.stack_offset]);
            else
                movq(r, Xmm(h.reg));
        } else if (h.spilled)
            mov(r, qword[rsp + h.stack_offset]);
        else if (in_call && (h.reg == 8 || h.reg == 9))
            mov(r, qword[rsp + allocation.call_offset + 192 + (h.reg - 8) * 8]);
        else if (r.getIdx() != h.reg)
            mov(r, Reg64(h.reg));
    } else if (c.Kind() == OperandKind::Immediate || c.Kind() == OperandKind::Condition ||
               c.Kind() == OperandKind::Flags || c.Kind() == OperandKind::Lambda)
        mov(r, c.Payload());
    else
        Reject(CompileError::UnsupportedOperand);
    if (!raw) Mask(r, std::min(64u, TypeBits(View(c))));
}

void Writer::ReadVector(CellView c, const Xmm& r) {
    if (c.Kind() == OperandKind::Value && source.Operations()[c.Payload()].type == ir::Type::V128) {
        const auto& h = allocation.homes[c.Payload()];
        if (h.spilled)
            movdqu(r, ptr[rsp + h.stack_offset]);
        else if (r.getIdx() != h.reg)
            movdqa(r, Xmm(h.reg));
    } else {
        Read(c, r10, true);
        movq(r, r10);
    }
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
    constexpr std::array<unsigned, 5> temp{1, 2, 6, 10, 11};
    while (depth < temp.size() && temp[depth] == unsigned(out.getIdx())) ++depth;
    if (depth >= temp.size()) Reject(CompileError::UnsupportedOperand);
    const Reg64 rhs(temp[depth]);
    Expression(at, end, rhs, depth + 1);
    switch (c.ExpressionOp()) {
        case OperandOp::Add:
            add(out, rhs);
            break;
        case OperandOp::Sub:
            sub(out, rhs);
            break;
        case OperandOp::AddShift:
            if (c.ShiftAmount()) shl(rhs, c.ShiftAmount());
            add(out, rhs);
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
    mov(rax, c.Arg(0).Payload());
    if (c.vector_result) movq(xmm0, rax);
}

void Writer::Handle(Op<OpCode::Zero>, OpContext& c) {
    if (c.vector_result)
        pxor(xmm0, xmm0);
    else
        xor_(eax, eax);
}

void Writer::Handle(Op<OpCode::BitCast>, OpContext& c) {
    if (c.vector_result)
        ReadVector(c.Arg(0), xmm0);
    else
        Read(c.Arg(0), rax, true);
}

void Writer::Handle(Op<OpCode::GetResult>, OpContext& c) { Handle(Op<OpCode::BitCast>{}, c); }

void Writer::Handle(Op<OpCode::GetOperand>, OpContext& c) {
    auto a = c.operands.begin();
    Expression(a, c.operands.end(), rax);
}

void Writer::Handle(Op<OpCode::ZeroExtend32>, OpContext& c) {
    Read(c.Arg(0), rax);
    Mask(rax, 32);
}

void Writer::Handle(Op<OpCode::ZeroExtend32To64>, OpContext& c) {
    Handle(Op<OpCode::ZeroExtend32>{}, c);
}

void Writer::Handle(Op<OpCode::ZeroExtend64>, OpContext& c) { Read(c.Arg(0), rax); }

void Writer::Handle(Op<OpCode::SignExtend>, OpContext& c) {
    Read(c.Arg(0), rax);
    Sign(rax, TypeBits(View(c.Arg(0))));
}
}  // namespace swift::runtime::backend::x64::detail
