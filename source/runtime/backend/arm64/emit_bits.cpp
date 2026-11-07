#include "emit.h"
#include "runtime/arch/x86/bit_operations.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {
std::uintptr_t CpuidHelper() noexcept;

namespace detail {
namespace {
// Same pair-return C ABI as CpuidHelper, with no CPU/state observation.
struct BitResult {
    u64 low, high;
};

BitResult DepositBitsCallback(u64 value, u64 mask) noexcept {
    return {x86::ParallelBitDeposit(value, mask), 0};
}

BitResult ExtractBitsCallback(u64 value, u64 mask) noexcept {
    return {x86::ParallelBitExtract(value, mask), 0};
}
}  // namespace

void Writer::HandleParallelBitsFamily(OpContext& ctx) {
    if (ctx.width != 32 && ctx.width != 64) Reject(CompileError::UnsupportedType);
    const auto value = ctx.cursor.Take(), mask = ctx.cursor.Take();
    // Read original spill slots before this operation owns a temporary stack
    // cell. The existing private bridge preserves every scalar/vector home.
    Move(0, Read(value, 0, ctx.width));
    Move(16, Read(mask, 16, ctx.width));
    __ mrs(v::x17, v::NZCV);
    __ sub(v::sp, v::sp, Operand(16));
    Memory(false, 64, 17, 31);
    const auto callback = ctx.operation.opcode == OpCode::ParallelBitDeposit ? &DepositBitsCallback
                                                                             : &ExtractBitsCallback;
    Immediate(17, reinterpret_cast<std::uintptr_t>(callback));
    Immediate(30, CpuidHelper());
    __ blr(v::x30);
    Memory(true, 64, 17, 31);
    __ add(v::sp, v::sp, Operand(16));
    __ msr(v::NZCV, v::x17);
    // Restore the exact incoming PSTATE, including a still-direct carry.
    // Pure numerical helpers do not pack guest flags or publish state.
    if (ctx.width == 32)
        __ mov(W(ctx.destination), v::w0);
    else
        Move(ctx.destination, 0);
}
}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
