#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {
void Writer::HandleCompareAndSwap128Family(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    const auto address = cursor.Take(), expected_low = cursor.Take(), expected_high = cursor.Take();
    const auto desired_low = cursor.Take(), desired_high = cursor.Take();
    for (const auto input : {address, expected_low, expected_high, desired_low, desired_high})
        if (Width(View(input)) != 64) Reject(CompileError::UnsupportedOperand);
    if (!options.private_uniforms) Shadow(false);
    // v0/v1 and x0/x16/x17/x30 are the explicit writer scratch bank.
    // Keep the PF/AF carriers untouched at every faultable instruction.
    __ ins(Lanes(0, 64), 0, X(Read(expected_low, 16)));
    __ ins(Lanes(0, 64), 1, X(Read(expected_high, 16)));
    __ ins(Lanes(1, 64), 0, X(Read(desired_low, 16)));
    __ ins(Lanes(1, 64), 1, X(Read(desired_high, 16)));
    Move(0, Read(address, 0));
    const MemOperand memory(v::x0);
    const auto retry = masm.Here();
    __ ldaxp(v::x16, v::x17, memory);
    __ umov(v::x30, Lanes(0, 64), 0);
    __ eor(v::x30, v::x30, Operand(v::x16));
    const auto low_mismatch = masm.Here();
    __ cbnz(v::x30, std::int64_t{0});
    __ umov(v::x30, Lanes(0, 64), 1);
    __ eor(v::x30, v::x30, Operand(v::x17));
    const auto high_mismatch = masm.Here();
    __ cbnz(v::x30, std::int64_t{0});
    __ umov(v::x16, Lanes(1, 64), 0);
    __ umov(v::x17, Lanes(1, 64), 1);
    __ stlxp(v::w30, v::x16, v::x17, memory);
    __ cbnz(v::w30, static_cast<std::int64_t>(retry) - masm.Here());
    VectorMove(ctx.destination, 0);
    const auto done = masm.Here();
    __ b(std::int64_t{0});
    masm.Patch(low_mismatch, masm.Here());
    masm.Patch(high_mismatch, masm.Here());
    // A failed comparison retains the locked write cycle, including its
    // permission and SMC checks. Failed exclusive stores restart the load.
    __ stlxp(v::w30, v::x16, v::x17, memory);
    __ cbnz(v::w30, static_cast<std::int64_t>(retry) - masm.Here());
    __ ins(Lanes(ctx.destination, 64), 0, v::x16);
    __ ins(Lanes(ctx.destination, 64), 1, v::x17);
    masm.Patch(done, masm.Here());
    SaveResult(ctx);
    if (!options.private_uniforms) Shadow(true);
}

namespace {
void EmitLseAtomic(v::Assembler& sink,
                   AtomicRMWOp kind,
                   bool exchange,
                   unsigned bits,
                   Register value,
                   Register previous,
                   const MemOperand& memory) {
    if (exchange) {
        if (bits == 8)
            sink.swpalb(value, previous, memory);
        else if (bits == 16)
            sink.swpalh(value, previous, memory);
        else
            sink.swpal(value, previous, memory);
    } else if (kind == AtomicRMWOp::And) {
        if (bits == 8)
            sink.ldclralb(value, previous, memory);
        else if (bits == 16)
            sink.ldclralh(value, previous, memory);
        else
            sink.ldclral(value, previous, memory);
    } else if (kind == AtomicRMWOp::Or) {
        if (bits == 8)
            sink.ldsetalb(value, previous, memory);
        else if (bits == 16)
            sink.ldsetalh(value, previous, memory);
        else
            sink.ldsetal(value, previous, memory);
    } else if (kind == AtomicRMWOp::Xor) {
        if (bits == 8)
            sink.ldeoralb(value, previous, memory);
        else if (bits == 16)
            sink.ldeoralh(value, previous, memory);
        else
            sink.ldeoral(value, previous, memory);
    } else if (kind == AtomicRMWOp::SignedMax) {
        if (bits == 8)
            sink.ldsmaxalb(value, previous, memory);
        else if (bits == 16)
            sink.ldsmaxalh(value, previous, memory);
        else
            sink.ldsmaxal(value, previous, memory);
    } else if (kind == AtomicRMWOp::SignedMin) {
        if (bits == 8)
            sink.ldsminalb(value, previous, memory);
        else if (bits == 16)
            sink.ldsminalh(value, previous, memory);
        else
            sink.ldsminal(value, previous, memory);
    } else if (kind == AtomicRMWOp::UnsignedMax) {
        if (bits == 8)
            sink.ldumaxalb(value, previous, memory);
        else if (bits == 16)
            sink.ldumaxalh(value, previous, memory);
        else
            sink.ldumaxal(value, previous, memory);
    } else if (kind == AtomicRMWOp::UnsignedMin) {
        if (bits == 8)
            sink.lduminalb(value, previous, memory);
        else if (bits == 16)
            sink.lduminalh(value, previous, memory);
        else
            sink.lduminal(value, previous, memory);
    } else {
        if (bits == 8)
            sink.ldaddalb(value, previous, memory);
        else if (bits == 16)
            sink.ldaddalh(value, previous, memory);
        else
            sink.ldaddal(value, previous, memory);
    }
}

void SelectMinMax(v::Assembler& sink, AtomicRMWOp kind, unsigned bits) {
    const v::Register old = bits == 64 ? v::Register(v::x16) : v::Register(v::w16);
    const v::Register input = bits == 64 ? v::Register(v::x17) : v::Register(v::w17);
    const bool sign = kind == AtomicRMWOp::SignedMax || kind == AtomicRMWOp::SignedMin;
    const bool maximum = kind == AtomicRMWOp::SignedMax || kind == AtomicRMWOp::UnsignedMax;
    // This new comparison must leave the live guest flags and observed old
    // value intact, including narrow signed operations.
    sink.mrs(v::x30, v::NZCV);
    sink.fmov(v::d0, v::x30);
    if (sign && bits < 32) {
        if (bits == 8) {
            sink.sxtb(old, old);
            sink.sxtb(input, input);
        } else {
            sink.sxth(old, old);
            sink.sxth(input, input);
        }
    }
    sink.cmp(old, v::Operand(input));
    sink.csel(input, old, input, sign ? (maximum ? v::gt : v::lt) : (maximum ? v::hi : v::lo));
    if (sign && bits < 32) sink.ubfx(old, old, 0, bits);
    sink.fmov(v::x30, v::d0);
    sink.msr(v::NZCV, v::x30);
}
}  // namespace

void Writer::HandleCompareAndSwapFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    const auto save_result = [&] { SaveResult(ctx); };
    if (width != 8 && width != 16 && width != 32 && width != 64)
        Reject(CompileError::UnsupportedType);
    const bool compare = operation.opcode == OpCode::CompareAndSwap;
    const bool exchange = operation.opcode == OpCode::AtomicExchange;
    auto kind = AtomicRMWOp::Add;
    if (operation.opcode == OpCode::AtomicRMW) {
        const auto input = IntegerImmediate(cursor.Take());
        if (input > static_cast<u64>(AtomicRMWOp::UnsignedMin))
            Reject(CompileError::UnsupportedOperand);
        kind = static_cast<AtomicRMWOp>(input);
    }
    const auto address = cursor.Take(), input = cursor.Take();
    const auto extra =
            compare || operation.opcode == OpCode::AtomicRMW ? cursor.Take() : CellView{};
    if (!options.private_uniforms) Shadow(false);
    Move(0, Read(address, 0));
    const auto native_width = width == 64 ? 64u : 32u;
    const MemOperand memory(v::x0);
    const auto load_exclusive = [&] {
        if (width == 8)
            __ ldaxrb(v::w16, memory);
        else if (width == 16)
            __ ldaxrh(v::w16, memory);
        else
            __ ldaxr(R(16, native_width), memory);
    };
    const auto store_exclusive = [&](unsigned value) {
        if (width == 8)
            __ stlxrb(v::w30, W(value), memory);
        else if (width == 16)
            __ stlxrh(v::w30, W(value), memory);
        else
            __ stlxr(v::w30, R(value, native_width), memory);
    };
    const auto retry_branch = [&](u32 target) {
        __ cbnz(v::w30, static_cast<std::int64_t>(target) - masm.Here());
    };
    if (compare) {
        Move(17, Read(extra, 17, width, false, native_width));
        if (options.lse_atomics) {
            Move(16, Read(input, 16, width, false, native_width));
            if (width == 8)
                __ casalb(v::w16, v::w17, memory);
            else if (width == 16)
                __ casalh(v::w16, v::w17, memory);
            else
                __ casal(R(16, native_width),
                         R(17, native_width),
                         memory);  // CASAL expected/old16,desired17,[x0].
        } else {
            const auto retry = static_cast<u32>(output.words.size());
            Move(30, Read(input, 30, width, false, native_width));
            load_exclusive();
            __ eor(R(30, native_width),
                   R(16, native_width),
                   Operand(R(30, native_width)));  // EOR expected^old.
            const auto mismatch = output.words.size();
            __ cbnz(R(30, native_width), std::int64_t{0});
            store_exclusive(17);
            retry_branch(retry);
            const auto done = masm.Here();
            __ b(std::int64_t{0});
            masm.Patch(mismatch, masm.Here());
            // A failed comparison still performs a write cycle.
            // Do not bypass write permission/SMC faults with CLREX.
            store_exclusive(16);
            retry_branch(retry);
            masm.Patch(done, masm.Here());
        }
    } else {
        const auto prepare = [&] {
            if (exchange || kind != AtomicRMWOp::Neg)
                Move(17, Read(input, 17, width, false, native_width));
            if (kind == AtomicRMWOp::AddCarry || kind == AtomicRMWOp::SubBorrow)
                Move(30, Read(extra, 30, 1));
        };
        if (options.lse_atomics && kind != AtomicRMWOp::Neg) {
            prepare();
            if (!exchange) switch (kind) {
                    case AtomicRMWOp::Add:
                        break;
                    case AtomicRMWOp::Sub:
                        __ neg(R(17, native_width), Operand(R(17, native_width)));
                        break;
                    case AtomicRMWOp::And:
                        __ mvn(R(17, native_width), Operand(R(17, native_width)));
                        break;  // LDCLRAL with ~operand.
                    case AtomicRMWOp::Or:
                        break;
                    case AtomicRMWOp::Xor:
                        break;
                    case AtomicRMWOp::AddCarry:
                        __ add(R(17, native_width),
                               R(17, native_width),
                               Operand(R(30, native_width)));
                        break;
                    case AtomicRMWOp::SubBorrow:
                        __ add(R(17, native_width),
                               R(17, native_width),
                               Operand(R(30, native_width)));
                        __ neg(R(17, native_width), Operand(R(17, native_width)));
                        break;
                    case AtomicRMWOp::Neg:
                        break;  // Uses the shared exclusive loop below.
                    case AtomicRMWOp::SignedMax:
                    case AtomicRMWOp::SignedMin:
                    case AtomicRMWOp::UnsignedMax:
                    case AtomicRMWOp::UnsignedMin:
                        break;
                }
            // Exact byte/halfword/register forms; the shared address
            // and acquire/release ordering are unchanged.
            EmitLseAtomic(masm.exact,
                          kind,
                          exchange,
                          width,
                          R(17, native_width),
                          R(16, native_width),
                          memory);
        } else {
            const auto retry = static_cast<u32>(output.words.size());
            // Reload operand/carry on retry: value17 and carry30
            // become the attempted update and store status.
            prepare();
            load_exclusive();
            if (!exchange) switch (kind) {
                    case AtomicRMWOp::Add:
                    case AtomicRMWOp::AddCarry:
                        __ add(R(17, native_width),
                               R(16, native_width),
                               Operand(R(17, native_width)));
                        break;
                    case AtomicRMWOp::Sub:
                    case AtomicRMWOp::SubBorrow:
                        __ sub(R(17, native_width),
                               R(16, native_width),
                               Operand(R(17, native_width)));
                        break;
                    case AtomicRMWOp::And:
                        __ and_(R(17, native_width),
                                R(16, native_width),
                                Operand(R(17, native_width)));
                        break;
                    case AtomicRMWOp::Or:
                        __ orr(R(17, native_width),
                               R(16, native_width),
                               Operand(R(17, native_width)));
                        break;
                    case AtomicRMWOp::Xor:
                        __ eor(R(17, native_width),
                               R(16, native_width),
                               Operand(R(17, native_width)));
                        break;
                    case AtomicRMWOp::Neg:
                        __ neg(R(17, native_width), Operand(R(16, native_width)));
                        break;
                    case AtomicRMWOp::SignedMax:
                    case AtomicRMWOp::SignedMin:
                    case AtomicRMWOp::UnsignedMax:
                    case AtomicRMWOp::UnsignedMin:
                        SelectMinMax(masm.exact, kind, width);
                        break;
                }
            if (kind == AtomicRMWOp::AddCarry)
                __ add(R(17, native_width), R(17, native_width), Operand(R(30, native_width)));
            else if (kind == AtomicRMWOp::SubBorrow)
                __ sub(R(17, native_width), R(17, native_width), Operand(R(30, native_width)));
            store_exclusive(17);
            retry_branch(retry);
        }
    }
    // Faultable instructions never publish their observed old
    // value into an architectural pin or the final SSA home.
    // Exclusive failures retry; only a completed RMW commits it.
    Move(destination, 16);
    save_result();
    if (!options.private_uniforms) Shadow(true);
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
