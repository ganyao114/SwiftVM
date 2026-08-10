#include <atomic>
#include <cstring>
#include "interpreter.h"
#include "runtime/backend/atomic_fallback.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::runtime::backend::interp {

using ir::ValueType;

#include "interpreter_internal.h"

void Interpreter::RunDefineLocal(ir::Inst* inst, InterpStack& stack) {}

void Interpreter::RunLoadLocal(ir::Inst* inst, InterpStack& stack) {
    // Locals are modelled as 8-byte slots indexed by Local::id inside
    // state.local_buffer (assumption: no current frontend emits locals; the
    // arm64 translator config sets has_local_operation=false and the JIT
    // PANICs on these).
    const auto local = inst->GetArg<ir::Local>(0);
    const auto* base = static_cast<const u8*>(state.local_buffer);
    u64 value{0};
    if (base) {
        std::memcpy(&value, base + size_t(local.id) * 8, ir::GetValueSizeByte(local.type));
    }
    WriteScalar(stack, inst, value);
}

void Interpreter::RunStoreLocal(ir::Inst* inst, InterpStack& stack) {
    const auto local = inst->GetArg<ir::Local>(0);
    auto* base = static_cast<u8*>(state.local_buffer);
    if (!base) {
        return;
    }
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    std::memcpy(base + size_t(local.id) * 8, &value, ir::GetValueSizeByte(local.type));
}

void Interpreter::RunLoadUniform(ir::Inst* inst, InterpStack& stack) {
    const auto uni = inst->GetArg<ir::Uniform>(0);
    const auto type = inst->ReturnType() == ValueType::VOID ? uni.GetType() : inst->ReturnType();
    const auto* base = &state.uniform_buffer_begin[uni.GetOffset()];
    if (IsVector(type)) {
        u128 value{};
        std::memcpy(&value, base, ir::GetValueSizeByte(type));
        WriteVec(stack, inst, value);
    } else {
        // Zero-extending load, matching the JIT's Ldrb/Ldrh/Ldr W.
        u64 value{0};
        std::memcpy(&value, base, ir::GetValueSizeByte(type));
        WriteScalar(stack, inst, value);
    }
}

void Interpreter::RunStoreUniform(ir::Inst* inst, InterpStack& stack) {
    const auto uni = inst->GetArg<ir::Uniform>(0);
    const auto value = inst->GetArg<ir::Value>(1);
    // Frontends always type their values; fall back to the uniform's declared
    // type for hand-built IR with an untyped value.
    const auto type = value.Type() == ValueType::VOID ? uni.GetType() : value.Type();
    auto* base = &state.uniform_buffer_begin[uni.GetOffset()];
    if (IsVector(type)) {
        const u128 v = ReadVec(stack, value);
        std::memcpy(base, &v, ir::GetValueSizeByte(type));
    } else {
        const u64 v = ReadScalar(stack, value);
        std::memcpy(base, &v, ir::GetValueSizeByte(type));
    }
}

void Interpreter::RunGetUniformAddress(ir::Inst* inst, InterpStack& stack) {
    const auto offset = inst->GetArg<ir::Imm>(0).Get();
    WriteScalar(stack,
                inst,
                reinterpret_cast<u64>(&state.uniform_buffer_begin[offset]));
}

void Interpreter::RunLoadMemory(ir::Inst* inst, InterpStack& stack) {
    const auto operand = inst->GetArg<ir::Operand>(0);
    const auto type = inst->ReturnType();
    u64 guest_addr = EvalOperand(stack, operand);
    // Wild-pointer guard: a guest address at or beyond the guest address-space
    // limit (Config::loc_end) is definitionally invalid. Raise PageFatal instead
    // of letting the host dereference a bad pointer (SIGSEGV). The JIT path
    // relies on the host signal handler for this; the interpreter has none.
    const u64 access_size = ir::GetValueSizeByte(type);
    // Bounded guest window first, so the interpreter validates (and faults on)
    // the same effective address the JIT would access -- the JIT truncates in
    // the addressing mode, so checking the untruncated address here would make
    // the two paths disagree about which wild pointers are in bounds.
    guest_addr &= state.guest_addr_mask;
    if (guest_addr >= state.guest_addr_limit || guest_addr + access_size > state.guest_addr_limit ||
        (state.interp_range_check &&
         !state.interp_range_check(state.interp_range_check_ctx, guest_addr, access_size))) {
        state.halt_reason = HaltReason::PageFatal;
        return;
    }
    // Guest address virtualization: state.pt carries the guest->host bias
    // (host = guest + bias); it is 0 for identity mapping.
    const auto* ptr =
            reinterpret_cast<const void*>(guest_addr + reinterpret_cast<uintptr_t>(state.pt));
    if (IsVector(type)) {
        u128 value{};
        std::memcpy(&value, ptr, ir::GetValueSizeByte(type));
        WriteVec(stack, inst, value);
    } else {
        // All scalar loads zero-extend (Ldrb/Ldrh/Ldr W/Ldr X in the JIT);
        // signed loads are expressed with a separate SignExtend instruction.
        u64 value{0};
        std::memcpy(&value, ptr, ir::GetValueSizeByte(type));
        WriteScalar(stack, inst, value);
    }
}

void Interpreter::RunStoreMemory(ir::Inst* inst, InterpStack& stack) {
    const auto operand = inst->GetArg<ir::Operand>(0);
    const auto value = inst->GetArg<ir::Value>(1);
    const auto type = value.Type();
    u64 guest_addr = EvalOperand(stack, operand);
    // Wild-pointer guard: see RunLoadMemory for the rationale.
    const u64 access_size = ir::GetValueSizeByte(type);
    // Bounded guest window first, so the interpreter validates (and faults on)
    // the same effective address the JIT would access -- the JIT truncates in
    // the addressing mode, so checking the untruncated address here would make
    // the two paths disagree about which wild pointers are in bounds.
    guest_addr &= state.guest_addr_mask;
    if (guest_addr >= state.guest_addr_limit || guest_addr + access_size > state.guest_addr_limit ||
        (state.interp_range_check &&
         !state.interp_range_check(state.interp_range_check_ctx, guest_addr, access_size))) {
        state.halt_reason = HaltReason::PageFatal;
        return;
    }
    auto* ptr = reinterpret_cast<void*>(guest_addr + reinterpret_cast<uintptr_t>(state.pt));
    if (IsVector(type)) {
        const u128 v = ReadVec(stack, value);
        std::memcpy(ptr, &v, ir::GetValueSizeByte(type));
    } else {
        const u64 v = ReadScalar(stack, value);
        std::memcpy(ptr, &v, ir::GetValueSizeByte(type));
    }
}

void Interpreter::RunLoadMemoryTSO(ir::Inst* inst, InterpStack& stack) {
    // TSO ordering is only observable with multiple concurrent guest threads;
    // the interpreter executes one guest thread on one host thread, so a TSO
    // load is semantically identical to a plain load here (the JIT provides
    // the ordering with plain load + dmb ishld — see
    // JitTranslator::EmitLoadMemoryTSO).
    RunLoadMemory(inst, stack);
}

void Interpreter::RunStoreMemoryTSO(ir::Inst* inst, InterpStack& stack) {
    // See RunLoadMemoryTSO: single-threaded execution makes the release
    // ordering unobservable, so TSO stores degrade to plain stores.
    RunStoreMemory(inst, stack);
}

void Interpreter::RunMemoryCopy(ir::Inst* inst, InterpStack& stack) {
    auto dst = inst->GetArg<ir::Lambda>(0);
    auto src = inst->GetArg<ir::Lambda>(1);
    const u64 size = inst->GetArg<ir::Imm>(2).Get();
    // The lambdas evaluate to guest addresses; apply the pt bias (0 for
    // identity mapping).
    const auto bias = reinterpret_cast<uintptr_t>(state.pt);
    const auto mask = state.guest_addr_mask;
    std::memmove(reinterpret_cast<void*>((EvalLambda(stack, dst) & mask) + bias),
                 reinterpret_cast<const void*>((EvalLambda(stack, src) & mask) + bias),
                 size);
}

void Interpreter::RunMemoryCopyTSO(ir::Inst* inst, InterpStack& stack) {
    RunMemoryCopy(inst, stack);
}

void Interpreter::RunMemoryBarrierTSO(ir::Inst* inst, InterpStack& stack) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

namespace {

template <typename T>
bool IsNaturallyAligned(const void* ptr) {
    constexpr auto required = std::atomic_ref<T>::required_alignment;
    return (reinterpret_cast<uintptr_t>(ptr) & (required - 1)) == 0;
}

template <typename T>
T AtomicCompareExchange(void* ptr, T expected, T desired) {
    if (IsNaturallyAligned<T>(ptr)) {
        std::atomic_ref(*static_cast<T*>(ptr))
                .compare_exchange_strong(expected, desired, std::memory_order_seq_cst);
        return expected;
    }
    runtime::backend::UnalignedAtomicGuard guard;
    T old;
    std::memcpy(&old, ptr, sizeof(old));
    if (old == expected) {
        std::memcpy(ptr, &desired, sizeof(desired));
    }
    return old;
}

template <typename T>
T AtomicExchangeValue(void* ptr, T desired) {
    if (IsNaturallyAligned<T>(ptr)) {
        return std::atomic_ref(*static_cast<T*>(ptr))
                .exchange(desired, std::memory_order_seq_cst);
    }
    runtime::backend::UnalignedAtomicGuard guard;
    T old;
    std::memcpy(&old, ptr, sizeof(old));
    std::memcpy(ptr, &desired, sizeof(desired));
    return old;
}

template <typename T>
T AtomicFetchAddValue(void* ptr, T addend) {
    if (IsNaturallyAligned<T>(ptr)) {
        return std::atomic_ref(*static_cast<T*>(ptr))
                .fetch_add(addend, std::memory_order_seq_cst);
    }
    runtime::backend::UnalignedAtomicGuard guard;
    T old;
    std::memcpy(&old, ptr, sizeof(old));
    const T desired = static_cast<T>(old + addend);
    std::memcpy(ptr, &desired, sizeof(desired));
    return old;
}

template <typename T, typename Transform>
T AtomicTransformValue(void* ptr, Transform&& transform) {
    if (IsNaturallyAligned<T>(ptr)) {
        auto ref = std::atomic_ref(*static_cast<T*>(ptr));
        T old = ref.load(std::memory_order_seq_cst);
        for (;;) {
            const T desired = transform(old);
            if (ref.compare_exchange_weak(old, desired, std::memory_order_seq_cst)) {
                return old;
            }
        }
    }
    runtime::backend::UnalignedAtomicGuard guard;
    T old;
    std::memcpy(&old, ptr, sizeof(old));
    const T desired = transform(old);
    std::memcpy(ptr, &desired, sizeof(desired));
    return old;
}

}  // namespace

void Interpreter::RunCompareAndSwap(ir::Inst* inst, InterpStack& stack) {
    // Args: (address, expected, desired); returns the old value.
    const u64 addr = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const auto expected = inst->GetArg<ir::Value>(1);
    const auto desired = inst->GetArg<ir::Value>(2);
    const u32 bits = TypeBits(expected.Type());
    const u64 mask = MaskBits(bits);
    auto* ptr = reinterpret_cast<void*>((addr & state.guest_addr_mask) + reinterpret_cast<uintptr_t>(state.pt));
    const u64 expected_value = ReadScalar(stack, expected) & mask;
    const u64 desired_value = ReadScalar(stack, desired) & mask;
    u64 old{};
    switch (bits) {
        case 8:
            old = AtomicCompareExchange(
                    ptr, static_cast<u8>(expected_value), static_cast<u8>(desired_value));
            break;
        case 16:
            old = AtomicCompareExchange(
                    ptr, static_cast<u16>(expected_value), static_cast<u16>(desired_value));
            break;
        case 32:
            old = AtomicCompareExchange(
                    ptr, static_cast<u32>(expected_value), static_cast<u32>(desired_value));
            break;
        case 64:
            old = AtomicCompareExchange(ptr, expected_value, desired_value);
            break;
        default:
            PANIC("unsupported CompareAndSwap width");
    }
    WriteScalar(stack, inst, old);
}

void Interpreter::RunCompareAndSwap128(ir::Inst* inst, InterpStack& stack) {
    const u64 addr = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 expected_lo = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    const u64 expected_hi = ReadScalar(stack, inst->GetArg<ir::Value>(2));
    const u64 desired_lo = ReadScalar(stack, inst->GetArg<ir::Value>(3));
    const u64 desired_hi = ReadScalar(stack, inst->GetArg<ir::Value>(4));
    auto* ptr = reinterpret_cast<void*>((addr & state.guest_addr_mask) + reinterpret_cast<uintptr_t>(state.pt));

    // std::atomic_ref<unsigned __int128> is neither portable nor guaranteed
    // lock-free. Serialize the 16-byte memcpy compare/store with the same
    // process-global lock used by the JIT's unaligned atomic slow path.
    runtime::backend::UnalignedAtomicGuard guard;
    u128 old{};
    std::memcpy(&old, ptr, sizeof(old));
    const u128 expected = (static_cast<u128>(expected_hi) << 64) | expected_lo;
    if (old == expected) {
        const u128 desired = (static_cast<u128>(desired_hi) << 64) | desired_lo;
        std::memcpy(ptr, &desired, sizeof(desired));
    }
    WriteVec(stack, inst, old);
}

void Interpreter::RunCheckMemoryAlignment(ir::Inst* inst, InterpStack& stack) {
    const u64 address = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 mask = inst->GetArg<ir::Imm>(1).Get();
    if ((address & mask) != 0) {
        state.halt_reason = HaltReason::PageFatal;
    }
}

void Interpreter::RunAtomicExchange(ir::Inst* inst, InterpStack& stack) {
    const u64 addr = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const auto desired = inst->GetArg<ir::Value>(1);
    auto* raw = reinterpret_cast<void*>((addr & state.guest_addr_mask) + reinterpret_cast<uintptr_t>(state.pt));
    const u64 value = ReadScalar(stack, desired);
    u64 old{};
    switch (TypeBits(desired.Type())) {
        case 8:
            old = AtomicExchangeValue(raw, static_cast<u8>(value));
            break;
        case 16:
            old = AtomicExchangeValue(raw, static_cast<u16>(value));
            break;
        case 32:
            old = AtomicExchangeValue(raw, static_cast<u32>(value));
            break;
        case 64:
            old = AtomicExchangeValue(raw, value);
            break;
        default:
            PANIC("unsupported AtomicExchange width");
    }
    WriteScalar(stack, inst, old);
}

void Interpreter::RunAtomicFetchAdd(ir::Inst* inst, InterpStack& stack) {
    const u64 addr = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const auto addend = inst->GetArg<ir::Value>(1);
    auto* raw = reinterpret_cast<void*>((addr & state.guest_addr_mask) + reinterpret_cast<uintptr_t>(state.pt));
    const u64 value = ReadScalar(stack, addend);
    u64 old{};
    switch (TypeBits(addend.Type())) {
        case 8:
            old = AtomicFetchAddValue(raw, static_cast<u8>(value));
            break;
        case 16:
            old = AtomicFetchAddValue(raw, static_cast<u16>(value));
            break;
        case 32:
            old = AtomicFetchAddValue(raw, static_cast<u32>(value));
            break;
        case 64:
            old = AtomicFetchAddValue(raw, value);
            break;
        default:
            PANIC("unsupported AtomicFetchAdd width");
    }
    WriteScalar(stack, inst, old);
}

void Interpreter::RunAtomicRMW(ir::Inst* inst, InterpStack& stack) {
    const auto op =
            static_cast<ir::AtomicRMWOp>(inst->GetArg<ir::Imm>(0).Get());
    const u64 addr = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    const auto operand_arg = inst->GetArg<ir::Value>(2);
    const auto carry_arg = inst->GetArg<ir::Value>(3);
    void* raw = reinterpret_cast<void*>((addr & state.guest_addr_mask) + reinterpret_cast<uintptr_t>(state.pt));
    const u64 operand = ReadScalar(stack, operand_arg);
    const u64 carry = ReadScalar(stack, carry_arg) & 1;

    const auto transform = [=](auto old) {
        using T = decltype(old);
        const T rhs = static_cast<T>(operand);
        switch (op) {
            case ir::AtomicRMWOp::Add:
                return static_cast<T>(old + rhs);
            case ir::AtomicRMWOp::Sub:
                return static_cast<T>(old - rhs);
            case ir::AtomicRMWOp::And:
                return static_cast<T>(old & rhs);
            case ir::AtomicRMWOp::Or:
                return static_cast<T>(old | rhs);
            case ir::AtomicRMWOp::Xor:
                return static_cast<T>(old ^ rhs);
            case ir::AtomicRMWOp::Neg:
                return static_cast<T>(T(0) - old);
            case ir::AtomicRMWOp::AddCarry:
                return static_cast<T>(old + rhs + static_cast<T>(carry));
            case ir::AtomicRMWOp::SubBorrow:
                return static_cast<T>(old - rhs - static_cast<T>(carry));
        }
        return old;
    };

    u64 old{};
    switch (TypeBits(operand_arg.Type())) {
        case 8:
            old = AtomicTransformValue<u8>(raw, transform);
            break;
        case 16:
            old = AtomicTransformValue<u16>(raw, transform);
            break;
        case 32:
            old = AtomicTransformValue<u32>(raw, transform);
            break;
        case 64:
            old = AtomicTransformValue<u64>(raw, transform);
            break;
        default:
            PANIC("unsupported AtomicRMW width");
    }
    WriteScalar(stack, inst, old);
}

void Interpreter::RunUniformBarrier(ir::Inst* inst, InterpStack& stack) {
    // Compiler barrier only (same as the JIT); no runtime effect.
}

}  // namespace swift::runtime::backend::interp
