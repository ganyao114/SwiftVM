#include "translator.h"

#include <cstring>
#include <stdexcept>
#include "runtime/backend/atomic_fallback.h"
#include "runtime/common/host_pair_result.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;
using R = ir::AtomicRMWOp;

namespace {

enum class AtomicKind { Exchange, Add, And, Or, Xor, Neg, Compare };

template<class T, AtomicKind kind>
u64 UnalignedAtomic(void* pointer, u64 operand, u64 desired) noexcept {
    // No hardware instruction implements a misaligned AMO. This slow kernel
    // uses the existing shared lock and can be abandoned by fault recovery.
    UnalignedAtomicGuard guard;
    T old;
    std::memcpy(&old, pointer, sizeof(old));
    T replacement;
    if constexpr (kind == AtomicKind::Exchange) replacement = T(operand);
    if constexpr (kind == AtomicKind::Add) replacement = T(old + T(operand));
    if constexpr (kind == AtomicKind::And) replacement = T(old & T(operand));
    if constexpr (kind == AtomicKind::Or) replacement = T(old | T(operand));
    if constexpr (kind == AtomicKind::Xor) replacement = T(old ^ T(operand));
    if constexpr (kind == AtomicKind::Neg) replacement = T(T(0) - old);
    if constexpr (kind == AtomicKind::Compare) replacement = old == T(operand) ? T(desired) : old;
    std::memcpy(pointer, &replacement, sizeof(replacement));
    return old;
}

using AtomicKernel = u64 (*)(void*, u64, u64);

HostPairResult CompareWide(void* pointer, u64 expected_low, u64 expected_high,
                          u64 desired_low, u64 desired_high) noexcept {
    UnalignedAtomicGuard guard;
    HostPairResult old{};
    std::memcpy(&old, pointer, 16);
    if (old.first == expected_low && old.second == expected_high) {
        const HostPairResult desired{desired_low, desired_high};
        std::memcpy(pointer, &desired, 16);
    }
    return old;
}

template<class T>
AtomicKernel Kernel(AtomicKind kind) {
    switch (kind) {
        case AtomicKind::Exchange: return &UnalignedAtomic<T, AtomicKind::Exchange>;
        case AtomicKind::Add: return &UnalignedAtomic<T, AtomicKind::Add>;
        case AtomicKind::And: return &UnalignedAtomic<T, AtomicKind::And>;
        case AtomicKind::Or: return &UnalignedAtomic<T, AtomicKind::Or>;
        case AtomicKind::Xor: return &UnalignedAtomic<T, AtomicKind::Xor>;
        case AtomicKind::Neg: return &UnalignedAtomic<T, AtomicKind::Neg>;
        case AtomicKind::Compare: return &UnalignedAtomic<T, AtomicKind::Compare>;
    }
    UNREACHABLE();
}

}  // namespace

bool JitTranslator::EmitAtomic(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::CompareAndSwap128: case O::CompareAndSwap: case O::AtomicExchange: case O::AtomicFetchAdd: case O::AtomicRMW: break;
        default: return false;
    }
    if (op == O::CompareAndSwap128) {
        const auto result = context.ResultPair(inst);
        auto& as = context.GetMasm();
        context.Read(a1, inst->GetArg<ir::Value>(0)); EmitAddress(16);
        for (u32 i = 1; i < 5; ++i) context.Read(GPR{11 + i}, inst->GetArg<ir::Value>(i));
        as.FENCE(FenceOrder::RW, FenceOrder::RW);
        Label slow, done;
        if (context.Features().zacas) {
            as.ANDI(t2, a0, 15); as.BNE(t2, x0, &slow);
            // Zacas requires even expected and desired register pairs.
            as.AMOCAS_Q(Ordering::AQRL, a2, a4, a0);
            as.MV(result[0], a2); as.MV(result[1], a3); as.J(&done); as.Bind(&slow);
        }
        as.MV(a1, a2); as.MV(a2, a3); as.MV(a3, a4); as.MV(a4, a5);
        context.SaveVectorsForCall();
        context.HostAddress(t0, reinterpret_cast<u64>(&CompareWide)); context.LeaveFloatMode(); as.JALR(t0); context.EnterFloatMode();
        as.MV(result[0], a0); as.MV(result[1], a1);
        context.ReloadFlags(); context.RestoreVectorsAfterCall();
        if (context.Features().zacas) { as.Bind(&done); context.ResetVectorType(); }
        as.FENCE(FenceOrder::RW, FenceOrder::RW); context.WritePair(inst, result);
        return true;
    }
    const bool rmw = op == O::AtomicRMW;
    const auto input = inst->GetArg<ir::Value>(rmw ? 2 : 1);
    const u32 size = ir::GetValueSizeByte(input.Type());
    if (ir::IsFloatValueType(input.Type()) || size > 8) return false;
    const auto result = context.ResultRegister(inst);
    auto& as = context.GetMasm();
    context.Read(a1, inst->GetArg<ir::Value>(rmw ? 1 : 0));
    EmitAddress(size);
    context.Read(a1, input); context.Mask(a1, size * 8);
    AtomicKind kind = op == O::CompareAndSwap ? AtomicKind::Compare :
                      op == O::AtomicExchange ? AtomicKind::Exchange : AtomicKind::Add;
    if (op == O::CompareAndSwap) {
        context.Read(a2, inst->GetArg<ir::Value>(2)); context.Mask(a2, size * 8);
    } else if (rmw) {
        const auto operation = static_cast<R>(inst->GetArg<ir::Imm>(0).Get());
        if (operation == R::AddCarry || operation == R::SubBorrow) {
            context.Read(t0, inst->GetArg<ir::Value>(3)); as.ANDI(t0, t0, 1); as.ADD(a1, a1, t0);
        }
        switch (operation) {
            case R::Sub: case R::SubBorrow: as.NEG(a1, a1); break;
            case R::And: kind = AtomicKind::And; break;
            case R::Or: kind = AtomicKind::Or; break;
            case R::Xor: kind = AtomicKind::Xor; break;
            case R::Neg: kind = AtomicKind::Neg; break;
            case R::Add: case R::AddCarry: break;
            default: throw std::runtime_error("invalid RV64 atomic RMW operation");
        }
    }
    Label slow, done;
    // SC failure and compare mismatch must also participate in seq_cst
    // ordering. The full fences cover ordinary guest accesses on either side.
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
    if (size > 1) { as.ANDI(t0, a0, size - 1); as.BNE(t0, x0, &slow); }
    if (size >= 4 && kind != AtomicKind::Compare && kind != AtomicKind::Neg) {
        const auto emit = [&](auto word, auto wide) {
            if (size == 4) (as.*word)(Ordering::AQRL, result, a1, a0);
            else (as.*wide)(Ordering::AQRL, result, a1, a0);
        };
        switch (kind) {
            case AtomicKind::Exchange: emit(&Assembler::AMOSWAP_W, &Assembler::AMOSWAP_D); break;
            case AtomicKind::Add: emit(&Assembler::AMOADD_W, &Assembler::AMOADD_D); break;
            case AtomicKind::And: emit(&Assembler::AMOAND_W, &Assembler::AMOAND_D); break;
            case AtomicKind::Or: emit(&Assembler::AMOOR_W, &Assembler::AMOOR_D); break;
            case AtomicKind::Xor: emit(&Assembler::AMOXOR_W, &Assembler::AMOXOR_D); break;
            default: UNREACHABLE();
        }
    } else {
        // RV64A supports word reservations. Byte/halfword updates preserve
        // every neighboring bit of that word and retry when it changes. The
        // aligned word stays within the target's host page; a crossing
        // halfword uses the explicitly misaligned kernel below.
        if (size < 4) {
            as.ANDI(a3, a0, -4); as.ANDI(a4, a0, 3); as.SLLI(a4, a4, 3);
            as.LI(a5, (u64{1} << (size * 8)) - 1);
            as.SLL(a6, a5, a4); as.NOT(a6, a6);
        }
        Label retry, mismatch;
        as.Bind(&retry);
        if (size == 8) as.LR_D(Ordering::AQ, result, a0);
        else if (size == 4) { as.LR_W(Ordering::AQ, result, a0); context.Mask(result, 32); }
        else { as.LR_W(Ordering::AQ, t1, a3); as.SRL(result, t1, a4); as.AND(result, result, a5); }
        if (kind == AtomicKind::Compare) as.BNE(result, a1, &mismatch);
        switch (kind) {
            case AtomicKind::Exchange: as.MV(t2, a1); break;
            case AtomicKind::Compare: as.MV(t2, a2); break;
            case AtomicKind::Add: as.ADD(t2, result, a1); break;
            case AtomicKind::And: as.AND(t2, result, a1); break;
            case AtomicKind::Or: as.OR(t2, result, a1); break;
            case AtomicKind::Xor: as.XOR(t2, result, a1); break;
            case AtomicKind::Neg: as.NEG(t2, result); break;
        }
        if (size < 4) {
            as.AND(t2, t2, a5); as.SLL(t2, t2, a4); as.AND(t1, t1, a6); as.OR(t2, t2, t1);
            as.SC_W(Ordering::RL, t3, t2, a3);
        } else if (size == 4) as.SC_W(Ordering::RL, t3, t2, a0);
        else as.SC_D(Ordering::RL, t3, t2, a0);
        as.BNE(t3, x0, &retry);
        as.Bind(&mismatch);
    }
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
    as.J(&done);
    if (size > 1) {
        as.Bind(&slow);
        const auto kernel = size == 2 ? Kernel<u16>(kind) : size == 4 ? Kernel<u32>(kind) : Kernel<u64>(kind);
        context.SaveVectorsForCall();
        context.HostAddress(t0, reinterpret_cast<u64>(kernel)); context.LeaveFloatMode(); as.JALR(t0); context.EnterFloatMode();
        context.ReloadFlags();
        context.RestoreVectorsAfterCall();
        as.MV(result, a0);
        as.FENCE(FenceOrder::RW, FenceOrder::RW);
    }
    as.Bind(&done);
    context.ResetVectorType();
    context.Write(inst, result);
    return true;
}

}  // namespace swift::runtime::backend::riscv64
