#include "emit.h"
#include "runtime/atomic.h"
#include "runtime/exit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {
std::uintptr_t LambdaHelper() noexcept;

namespace detail {
u32 Writer::UnalignedAtomicService(OpContext& ctx,
                                   CellView input,
                                   CellView extra,
                                   AtomicRMWOp kind) {
    const auto bits = ctx.width;
    if (!Linked() || bits == 8) return UINT32_MAX;
    Extract(16, 0, 0, std::countr_zero(bits / 8));
    const auto aligned = masm.Here();
    __ cbz(v::x16, std::int64_t{0});

    const bool compare = ctx.operation.opcode == OpCode::CompareAndSwap;
    const bool rmw = ctx.operation.opcode == OpCode::AtomicRMW;
    constexpr AtomicOperation operations[]{AtomicOperation::Add,
                                           AtomicOperation::Sub,
                                           AtomicOperation::And,
                                           AtomicOperation::Or,
                                           AtomicOperation::Xor,
                                           AtomicOperation::Neg,
                                           AtomicOperation::AddCarry,
                                           AtomicOperation::SubBorrow,
                                           AtomicOperation::SignedMax,
                                           AtomicOperation::SignedMin,
                                           AtomicOperation::UnsignedMax,
                                           AtomicOperation::UnsignedMin};
    const auto operation = compare ? AtomicOperation::CompareExchange
                           : rmw   ? operations[static_cast<unsigned>(kind)]
                           : ctx.operation.opcode == OpCode::AtomicExchange
                                   ? AtomicOperation::Exchange
                                   : AtomicOperation::FetchAdd;
    // Capture SSA inputs before moving SP. v0/v1 are the writer scratch bank;
    // the existing fixed helper bridge retains all live SSA and pinned banks.
    __ fmov(v::d0, v::x0);
    __ ins(Lanes(0, 64), 1, X(Read(input, 16, bits)));
    const bool carry = rmw && (kind == AtomicRMWOp::AddCarry || kind == AtomicRMWOp::SubBorrow);
    if (compare || carry) __ fmov(v::d1, X(Read(extra, 16, compare ? bits : 1)));
    Shadow(false);
    PublishPinned();
    HelperFPEnvironment(false);
    constexpr auto flags_slot = sizeof(AtomicCall), status_slot = flags_slot + 8;
    constexpr auto frame_bytes = status_slot + 8;
    __ sub(v::sp, v::sp, Operand(frame_bytes));
    __ str(v::x1, MemOperand(v::sp, flags_slot));
    __ umov(v::x0, Lanes(0, 64), 0);
    __ str(v::x0, MemOperand(v::sp));
    Immediate(0, static_cast<u64>(operation) | (u64{bits} << 32));
    __ str(v::x0, MemOperand(v::sp, offsetof(AtomicRequest, operation)));
    __ stp(v::xzr, v::xzr, MemOperand(v::sp, offsetof(AtomicRequest, expected)));
    __ stp(v::xzr, v::xzr, MemOperand(v::sp, offsetof(AtomicRequest, desired)));
    __ str(v::xzr, MemOperand(v::sp, offsetof(AtomicRequest, carry)));
    __ umov(v::x0, Lanes(0, 64), 1);
    __ str(v::x0,
           MemOperand(
                   v::sp,
                   compare ? offsetof(AtomicRequest, expected) : offsetof(AtomicRequest, desired)));
    if (compare || carry) {
        __ fmov(v::x0, v::d1);
        __ str(v::x0,
               MemOperand(v::sp,
                          compare ? offsetof(AtomicRequest, desired)
                                  : offsetof(AtomicRequest, carry)));
    }
    ContextMemory(true, 0, abi::ContextOffset(SVM_A64_CTX_OPAQUE));
    __ mov(v::x16, v::sp);
    ContextMemory(true, 1, abi::ContextOffset(SVM_A64_CTX_ATOMIC_SERVICE));
    const auto missing = masm.Here();
    __ cbz(v::x1, std::int64_t{0});
    Immediate(17, 0);
    HostAddress(30, LambdaHelper(), HostSymbol::ArmLambdaBridge);
    __ blr(v::x30);
    const auto returned = masm.Here();
    __ b(std::int64_t{0});
    masm.Patch(missing, masm.Here());
    Immediate(0, static_cast<u64>(AtomicStatus::Unsupported));
    masm.Patch(returned, masm.Here());
    __ str(v::x0, MemOperand(v::sp, status_slot));
    __ ldr(v::x1, MemOperand(v::sp, flags_slot));
    HelperFPEnvironment(true);
    ReloadPinned();
    Shadow(true);
    RestoreHelperNZCV();
    nzcv_matches = 0;
    __ ldr(v::x0, MemOperand(v::sp, status_slot));
    const auto success = masm.Here();
    __ cbz(v::x0, std::int64_t{0});
    __ add(v::sp, v::sp, Operand(frame_bytes));
    __ sub(v::x0, v::x0, Operand(static_cast<u64>(AtomicStatus::Fault)));
    const auto unsupported = masm.Here();
    __ cbnz(v::x0, std::int64_t{0});
    if (has_fault_location && options.pc_offset != UINT32_MAX) {
        Immediate(0, fault_location);
        PCMemory(false);
    }
    Immediate(0, ExitValue(NativeExit::MemoryFault));
    const auto exit = masm.Here();
    __ b(std::int64_t{0});
    masm.Patch(unsupported, masm.Here());
    if (has_fault_location && options.pc_offset != UINT32_MAX) {
        Immediate(0, fault_location);
        PCMemory(false);
    }
    Immediate(0, ExitValue(NativeExit::UnsupportedAtomic));
    masm.Patch(exit, masm.Here());
    Epilogue();
    masm.Patch(success, masm.Here());
    __ ldr(v::x16, MemOperand(v::sp, offsetof(AtomicCall, observed)));
    __ add(v::sp, v::sp, Operand(frame_bytes));
    const auto done = masm.Here();
    __ b(std::int64_t{0});
    masm.Patch(aligned, masm.Here());
    return done;
}
}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
