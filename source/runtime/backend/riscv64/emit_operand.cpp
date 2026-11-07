#include "emit.h"

#include <algorithm>

namespace swift::runtime::backend::riscv64::detail {
Type Writer::View(CellView cell) const {
    if (cell.TypeView() != Type::Void) return cell.TypeView();
    if (cell.Kind() == OperandKind::Value) return source.Operations()[cell.Payload()].type;
    return Type::U64;
}

void Writer::Mask(GPR reg, unsigned bits) {
    if (!bits || bits >= 64) return;
    as.SLLI(reg, reg, 64 - bits);
    as.SRLI(reg, reg, 64 - bits);
}

void Writer::SignExtend(GPR reg, unsigned bits) {
    if (bits >= 64) return;
    as.SLLI(reg, reg, 64 - bits);
    as.SRAI(reg, reg, 64 - bits);
}

void Writer::Address(GPR destination, GPR base, std::int64_t offset) {
    if (offset >= -2048 && offset <= 2047)
        as.ADDI(destination, base, static_cast<int>(offset));
    else {
        as.LI(destination, static_cast<u64>(offset));
        as.ADD(destination, base, destination);
    }
}

void Writer::Load(GPR destination, GPR base, std::int32_t offset, unsigned bits) {
    if (offset < -2048 || offset > 2047) {
        Address(t6, base, offset);
        base = t6;
        offset = 0;
    }
    if (bits <= 8)
        as.LBU(destination, offset, base);
    else if (bits == 16)
        as.LHU(destination, offset, base);
    else if (bits == 32)
        as.LWU(destination, offset, base);
    else if (bits == 64)
        as.LD(destination, offset, base);
    else
        Reject(CompileError::UnsupportedType);
}

void Writer::Store(GPR value, GPR base, std::int32_t offset, unsigned bits) {
    if (offset < -2048 || offset > 2047) {
        Address(t6, base, offset);
        base = t6;
        offset = 0;
    }
    if (bits <= 8)
        as.SB(value, offset, base);
    else if (bits == 16)
        as.SH(value, offset, base);
    else if (bits == 32)
        as.SW(value, offset, base);
    else if (bits == 64)
        as.SD(value, offset, base);
    else
        Reject(CompileError::UnsupportedType);
}

void Writer::FrameLoad(GPR destination, u32 offset) { Load(destination, sp, offset, 64); }

void Writer::FrameStore(GPR value, u32 offset) { Store(value, sp, offset, 64); }

void Writer::Read(CellView cell, GPR destination, bool raw) {
    if (cell.Kind() == OperandKind::Value) {
        const auto& home = allocation.homes[cell.Payload()];
        if (home.spilled)
            FrameLoad(destination, home.stack_offset);
        else if (destination != GPR{home.reg})
            as.MV(destination, GPR{home.reg});
    } else if (cell.Kind() == OperandKind::Immediate || cell.Kind() == OperandKind::Flags ||
               cell.Kind() == OperandKind::Condition || cell.Kind() == OperandKind::Lambda)
        as.LI(destination, cell.Payload());
    else
        Reject(CompileError::UnsupportedOperand);
    if (!raw) Mask(destination, std::min(64u, TypeBits(View(cell))));
}

void Writer::ReadPair(CellView cell, GPR low, GPR high) {
    Read(cell, low, true);
    if (cell.Kind() == OperandKind::Value &&
        TypeBits(source.Operations()[cell.Payload()].type) == 128) {
        const auto& home = allocation.homes[cell.Payload()];
        if (home.spilled)
            FrameLoad(high, home.stack_offset + 8);
        else
            as.MV(high, GPR{home.high_reg});
    } else
        as.MV(high, x0);
}

void Writer::Expression(CellRange::Iterator& at,
                        CellRange::Iterator end,
                        GPR result,
                        unsigned depth) {
    if (at == end) Reject(CompileError::UnsupportedOperand);
    const auto cell = *at++;
    if (cell.Kind() != OperandKind::Expression) {
        Read(cell, result);
        return;
    }
    Expression(at, end, result, depth);
    if (cell.Count() == 1) return;
    constexpr std::array scratch{t0, t1, t2, t3, t4, t5};
    if (depth >= scratch.size()) Reject(CompileError::UnsupportedOperand);
    const auto right = scratch[depth];
    Expression(at, end, right, depth + 1);
    switch (cell.ExpressionOp()) {
        case OperandOp::Add:
            as.ADD(result, result, right);
            return;
        case OperandOp::Sub:
            as.SUB(result, result, right);
            return;
        case OperandOp::AddShift:
            if (cell.ShiftAmount()) as.SLLI(right, right, cell.ShiftAmount());
            as.ADD(result, result, right);
            return;
        case OperandOp::Lsl:
            as.SLL(result, result, right);
            return;
        case OperandOp::Lsr:
            as.SRL(result, result, right);
            return;
        case OperandOp::Asr:
            as.SRA(result, result, right);
            return;
        default:
            Reject(CompileError::UnsupportedOperand);
    }
}

void Writer::Handle(Op<OpCode::LoadImm>, OpContext& ctx) {
    as.LI(ctx.destination, ctx.Arg(0).Payload());
    if (ctx.operation.type == Type::V128)
        as.MV(ctx.high, x0);
    else
        Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::Zero>, OpContext& ctx) {
    as.MV(ctx.destination, x0);
    if (ctx.operation.type == Type::V128) as.MV(ctx.high, x0);
}

void Writer::Handle(Op<OpCode::BitCast>, OpContext& ctx) {
    ReadPair(ctx.Arg(0), a0, a1);
    as.MV(ctx.destination, a0);
    if (ctx.operation.type == Type::V128) as.MV(ctx.high, a1);
}

void Writer::Handle(Op<OpCode::GetResult>, OpContext& ctx) { Handle(Op<OpCode::BitCast>{}, ctx); }

void Writer::Handle(Op<OpCode::GetOperand>, OpContext& ctx) {
    auto at = ctx.operands.begin();
    Expression(at, ctx.operands.end(), ctx.destination);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::ZeroExtend32>, OpContext& ctx) {
    Read(ctx.Arg(0), ctx.destination);
    Mask(ctx.destination, 32);
}

void Writer::Handle(Op<OpCode::ZeroExtend32To64>, OpContext& ctx) {
    Handle(Op<OpCode::ZeroExtend32>{}, ctx);
}

void Writer::Handle(Op<OpCode::ZeroExtend64>, OpContext& ctx) { Read(ctx.Arg(0), ctx.destination); }

void Writer::Handle(Op<OpCode::SignExtend>, OpContext& ctx) {
    Read(ctx.Arg(0), ctx.destination);
    SignExtend(ctx.destination, std::min(64u, TypeBits(View(ctx.Arg(0)))));
    Mask(ctx.destination, ctx.width);
}
}  // namespace swift::runtime::backend::riscv64::detail
