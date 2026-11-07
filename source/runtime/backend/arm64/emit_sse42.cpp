#include "emit.h"
#include "vector_helper.h"
#include "runtime/arch/x86/sse42_string.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64::detail {
namespace {
namespace sse42 = ::swift::runtime::x86::sse42;

sse42::Block128 ImplicitString(u64 control, const sse42::Block128* arguments) noexcept {
    return {sse42::CompareImplicit(arguments[0], arguments[1], static_cast<std::uint8_t>(control)),
            0};
}

sse42::Block128 ExplicitString(u64 control, const sse42::Block128* arguments) noexcept {
    return {sse42::CompareExplicit(
                    arguments[0], arguments[1], arguments[2], static_cast<std::uint16_t>(control)),
            0};
}
}  // namespace

void Writer::HandleStringFamily(OpContext& ctx) {
    if (ctx.width != 64) Reject(CompileError::UnsupportedType);
    const bool explicit_lengths = ctx.operation.opcode == OpCode::Sse42StrExplicit;
    const auto left = ctx.cursor.Take(), right = ctx.cursor.Take();
    const auto lengths = explicit_lengths ? ctx.cursor.Take() : CellView{};
    const auto control = IntegerImmediate(ctx.cursor.Take());
    if (control > (explicit_lengths ? 511u : 255u)) Reject(CompileError::UnsupportedOperand);
    VectorMove(0, VectorValue(left, 0));
    VectorMove(1, VectorValue(right, 1));
    const auto temporary = VectorTemporary();
    if (explicit_lengths)
        VectorMove(temporary, VectorValue(lengths, temporary));
    else
        __ eor(Lanes(temporary, 8), Lanes(temporary, 8), Lanes(temporary, 8));
    __ sub(v::sp, v::sp, Operand(48));
    VectorMemory(false, 0, 31, 0);
    VectorMemory(false, 1, 31, 1);
    VectorMemory(false, temporary, 31, 2);
    __ add(v::x17, v::sp, Operand(0));
    Immediate(16, control);
    Immediate(
            0,
            reinterpret_cast<std::uintptr_t>(explicit_lengths ? &ExplicitString : &ImplicitString));
    Immediate(30, VectorHelperBridge());
    __ blr(v::x30);
    __ add(v::sp, v::sp, Operand(48));
    __ fmov(X(ctx.destination), D(0));
}

void Writer::HandleSse42FlagsFamily(OpContext& ctx) {
    CompleteLocalFlags();
    RequireFlags();
    const auto value = Read(ctx.cursor.Take(), 16);
    const auto mask = Flags(ctx.cursor.Take());
    if (NativeNZCV()) {
        if (mask & negative) Rmif(value, 21, 8);
        if (mask & zero) Rmif(value, 23, 4);
        if (mask & carry) {
            Rmif(value, 25, 2);
            __ cfinv();
        }
        if (mask & overflow) Rmif(value, 27, 1);
        packed_current = false;
    } else {
        const auto publish = [&](u64 flag, unsigned source_bit, unsigned destination_bit) {
            if (!(mask & flag)) return;
            Extract(17, value, source_bit, 1);
            Insert(1, 17, destination_bit, 1);
        };
        publish(negative, 24, 31);
        publish(zero, 25, 30);
        publish(carry, 26, 29);
        publish(overflow, 27, 28);
        nzcv_matches &= ~mask;
    }
    ClearMask(mask & (parity | auxiliary));
}
}  // namespace swift::runtime::backend::arm64::detail

#undef __
