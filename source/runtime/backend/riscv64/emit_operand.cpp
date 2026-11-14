#include "emit.h"

#include <algorithm>

namespace swift::runtime::backend::riscv64::detail {
namespace {
constexpr std::int64_t kImm12Min = -2048;
constexpr std::int64_t kImm12Max = 2047;

std::uint64_t MaskedImmediate(CellView cell, unsigned view_bits) noexcept {
    const u64 raw = cell.Payload();
    if (!view_bits || view_bits >= 64) return raw;
    return raw & ((u64{1} << view_bits) - 1);
}
}  // namespace

Type Writer::View(CellView cell) const {
    if (cell.TypeView() != Type::Void) return cell.TypeView();
    if (cell.Kind() == OperandKind::Value) return source.Operations()[cell.Payload()].type;
    return Type::U64;
}

void Writer::Mask(GPR reg, unsigned bits) {
    if (!bits || bits >= 64) return;
    if (bits <= 11) {
        as.ANDI(reg, reg, (1u << bits) - 1);
        return;
    }
    if (bits == 16 && options.zbb) {
        as.ZEXTH(reg, reg);
        return;
    }
    if (bits == 32 && options.zba) {
        as.ZEXTW(reg, reg);
        return;
    }
    as.SLLI(reg, reg, 64 - bits);
    as.SRLI(reg, reg, 64 - bits);
}

void Writer::SignExtend(GPR reg, unsigned bits) {
    if (!bits || bits >= 64) return;
    if (bits == 8 && options.zbb) {
        as.SEXTB(reg, reg);
        return;
    }
    if (bits == 16 && options.zbb) {
        as.SEXTH(reg, reg);
        return;
    }
    if (bits == 32) {
        as.ADDIW(reg, reg, 0);
        return;
    }
    as.SLLI(reg, reg, 64 - bits);
    as.SRAI(reg, reg, 64 - bits);
}

void Writer::Address(GPR destination, GPR base, std::int64_t offset) {
    if (offset == 0) {
        if (destination != base) as.MV(destination, base);
        return;
    }
    if (offset >= kImm12Min && offset <= kImm12Max)
        as.ADDI(destination, base, static_cast<int>(offset));
    else {
        as.LI(destination, static_cast<u64>(offset));
        as.ADD(destination, base, destination);
    }
}

void Writer::Load(GPR destination, GPR base, std::int32_t offset, unsigned bits) {
    if (offset < kImm12Min || offset > kImm12Max) {
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
    if (offset < kImm12Min || offset > kImm12Max) {
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
    const auto view_bits = std::min(64u, TypeBits(View(cell)));
    if (cell.Kind() == OperandKind::Value) {
        const auto& home = allocation.homes[cell.Payload()];
        const auto def_bits = std::min(64u, TypeBits(source.Operations()[cell.Payload()].type));
        if (home.spilled)
            FrameLoad(destination, home.stack_offset);
        else if (destination != GPR{home.reg})
            as.MV(destination, GPR{home.reg});
        if (!raw && view_bits < def_bits) Mask(destination, view_bits);
        return;
    }
    if (cell.Kind() == OperandKind::Immediate || cell.Kind() == OperandKind::Flags ||
        cell.Kind() == OperandKind::Condition || cell.Kind() == OperandKind::Lambda) {
        const u64 payload = raw ? cell.Payload() : MaskedImmediate(cell, view_bits);
        as.LI(destination, payload);
        return;
    }
    Reject(CompileError::UnsupportedOperand);
}

GPR Writer::ReadRef(CellView cell, GPR scratch, bool raw) {
    const auto view_bits = std::min(64u, TypeBits(View(cell)));
    if (cell.Kind() == OperandKind::Value) {
        const auto& home = allocation.homes[cell.Payload()];
        const auto def_bits = std::min(64u, TypeBits(source.Operations()[cell.Payload()].type));
        if (!home.spilled && (raw || view_bits >= def_bits)) return GPR{home.reg};
    } else if (cell.Kind() == OperandKind::Immediate || cell.Kind() == OperandKind::Flags ||
               cell.Kind() == OperandKind::Condition) {
        const u64 payload = raw ? cell.Payload() : MaskedImmediate(cell, view_bits);
        if (payload == 0) return x0;
    }
    Read(cell, scratch, raw);
    return scratch;
}

GPR Writer::ReadWidth(CellView cell, GPR scratch, unsigned width) {
    const auto view_bits = std::min({64u, width ? width : 64u, TypeBits(View(cell))});
    if (cell.Kind() == OperandKind::Value) {
        const auto& home = allocation.homes[cell.Payload()];
        const auto def_bits = std::min(64u, TypeBits(source.Operations()[cell.Payload()].type));
        if (!home.spilled && view_bits >= def_bits) return GPR{home.reg};
        if (home.spilled)
            FrameLoad(scratch, home.stack_offset);
        else if (scratch != GPR{home.reg})
            as.MV(scratch, GPR{home.reg});
        if (view_bits < def_bits) Mask(scratch, view_bits);
        return scratch;
    }
    if (cell.Kind() == OperandKind::Immediate || cell.Kind() == OperandKind::Flags ||
        cell.Kind() == OperandKind::Condition) {
        const u64 payload = MaskedImmediate(cell, view_bits);
        if (payload == 0) return x0;
        as.LI(scratch, payload);
        return scratch;
    }
    Read(cell, scratch);
    if (view_bits < 64) Mask(scratch, view_bits);
    return scratch;
}

void Writer::ReadPair(CellView cell, GPR low, GPR high) {
    Read(cell, low, true);
    if (cell.Kind() == OperandKind::Value &&
        TypeBits(source.Operations()[cell.Payload()].type) == 128) {
        const auto& home = allocation.homes[cell.Payload()];
        if (home.spilled)
            FrameLoad(high, home.stack_offset + 8);
        else if (high != GPR{home.high_reg})
            as.MV(high, GPR{home.high_reg});
    } else
        as.MV(high, x0);
}

void Writer::ReadExpr(CellRange::Iterator& at,
                      CellRange::Iterator end,
                      GPR destination,
                      unsigned width) {
    if (at == end) Reject(CompileError::UnsupportedOperand);
    if ((*at).Kind() != OperandKind::Expression) {
        const GPR ref = ReadWidth(*at++, destination, width);
        if (ref != destination) as.MV(destination, ref);
        return;
    }
    Expression(at, end, destination, 0);
    Mask(destination, width);
}

GPR Writer::ReadExprRef(CellRange::Iterator& at,
                        CellRange::Iterator end,
                        GPR scratch,
                        unsigned width) {
    if (at == end) Reject(CompileError::UnsupportedOperand);
    if ((*at).Kind() != OperandKind::Expression) return ReadWidth(*at++, scratch, width);
    Expression(at, end, scratch, 0);
    Mask(scratch, width);
    return scratch;
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
    if (at != end && (*at).Kind() == OperandKind::Immediate) {
        const auto imm_cell = *at;
        const auto imm = MaskedImmediate(imm_cell, std::min(64u, TypeBits(View(imm_cell))));
        const auto simm = static_cast<std::int64_t>(imm);
        switch (cell.ExpressionOp()) {
            case OperandOp::Add:
                if (simm >= kImm12Min && simm <= kImm12Max) {
                    ++at;
                    if (simm != 0) as.ADDI(result, result, static_cast<int>(simm));
                    return;
                }
                break;
            case OperandOp::Sub:
                if (simm != INT64_MIN && -simm >= kImm12Min && -simm <= kImm12Max) {
                    ++at;
                    if (simm != 0) as.ADDI(result, result, static_cast<int>(-simm));
                    return;
                }
                break;
            case OperandOp::Lsl:
                ++at;
                if (imm & 63) as.SLLI(result, result, static_cast<u32>(imm & 63));
                return;
            case OperandOp::Lsr:
                ++at;
                if (imm & 63) as.SRLI(result, result, static_cast<u32>(imm & 63));
                return;
            case OperandOp::Asr:
                ++at;
                if (imm & 63) as.SRAI(result, result, static_cast<u32>(imm & 63));
                return;
            default:
                break;
        }
    }
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
        case OperandOp::AddShift: {
            const auto shift = cell.ShiftAmount();
            if (shift == 0) {
                as.ADD(result, result, right);
            } else if (options.zba && shift == 1) {
                as.SH1ADD(result, right, result);
            } else if (options.zba && shift == 2) {
                as.SH2ADD(result, right, result);
            } else if (options.zba && shift == 3) {
                as.SH3ADD(result, right, result);
            } else {
                as.SLLI(right, right, shift);
                as.ADD(result, result, right);
            }
            return;
        }
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
    const u64 payload = ctx.operation.type == Type::V128 || ctx.width >= 64 || !ctx.width
                                ? ctx.Arg(0).Payload()
                                : (ctx.Arg(0).Payload() & ((u64{1} << ctx.width) - 1));
    as.LI(ctx.destination, payload);
    if (ctx.operation.type == Type::V128) as.MV(ctx.high, x0);
}

void Writer::Handle(Op<OpCode::Zero>, OpContext& ctx) {
    as.MV(ctx.destination, x0);
    if (ctx.operation.type == Type::V128) as.MV(ctx.high, x0);
}

void Writer::Handle(Op<OpCode::BitCast>, OpContext& ctx) {
    if (ctx.operation.type == Type::V128) {
        ReadPair(ctx.Arg(0), ctx.destination, ctx.high);
        return;
    }
    Read(ctx.Arg(0), ctx.destination, true);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::GetResult>, OpContext& ctx) { Handle(Op<OpCode::BitCast>{}, ctx); }

void Writer::Handle(Op<OpCode::GetOperand>, OpContext& ctx) {
    auto at = ctx.operands.begin();
    ReadExpr(at, ctx.operands.end(), ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::ZeroExtend32>, OpContext& ctx) {
    const GPR src = ReadRef(ctx.Arg(0), ctx.destination);
    if (src != ctx.destination) {
        if (options.zba) {
            as.ZEXTW(ctx.destination, src);
            return;
        }
        as.MV(ctx.destination, src);
    }
    Mask(ctx.destination, 32);
}

void Writer::Handle(Op<OpCode::ZeroExtend32To64>, OpContext& ctx) {
    Handle(Op<OpCode::ZeroExtend32>{}, ctx);
}

void Writer::Handle(Op<OpCode::ZeroExtend64>, OpContext& ctx) { Read(ctx.Arg(0), ctx.destination); }

void Writer::Handle(Op<OpCode::SignExtend>, OpContext& ctx) {
    const auto from_bits = std::min(64u, TypeBits(View(ctx.Arg(0))));
    const GPR src = ReadRef(ctx.Arg(0), ctx.destination, true);
    if (from_bits == 32) {
        as.ADDIW(ctx.destination, src, 0);
    } else if (from_bits == 8 && options.zbb) {
        as.SEXTB(ctx.destination, src);
    } else if (from_bits == 16 && options.zbb) {
        as.SEXTH(ctx.destination, src);
    } else {
        if (src != ctx.destination) as.MV(ctx.destination, src);
        SignExtend(ctx.destination, from_bits);
    }
    Mask(ctx.destination, ctx.width);
}
}  // namespace swift::runtime::backend::riscv64::detail
