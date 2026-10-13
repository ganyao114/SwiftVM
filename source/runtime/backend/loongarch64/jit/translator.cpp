#include "translator.h"
#include "helpers.h"
#include "fault_scope.h"
#include "wide.h"

#include <algorithm>
#include <cfenv>
#include <cstring>
#include <stdexcept>
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/interrupt_request.h"
#include "runtime/backend/single_instruction_control_flow.h"
#include "runtime/common/saturating_add_sub.h"
#include "runtime/common/unsigned_estimate.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
using SemanticHelper = void (*)(State*, ir::Block*, ir::Inst*, u64*, u64) noexcept;
using U128 = unsigned __int128;

HaltReason ObserveSingleEntry(State* current) noexcept {
    return ObserveHaltRequest(*current);
}

bool AuditedGuestMemoryOpcode(O op) {
    switch (op) {
        case O::LoadMemory: case O::LoadMemoryTSO: case O::StoreMemory: case O::StoreMemoryTSO:
        case O::MemoryCopy: case O::MemoryCopyTSO: case O::CompareAndSwap: case O::CompareAndSwap128:
        case O::AtomicExchange: case O::AtomicFetchAdd: case O::AtomicRMW: return true;
        default: return false;
    }
}

// These accessors have the same canonical scalar/view rules as InterpStack.
// All IR operands are read while the guest fault scope is inactive.
struct KernelValues {
    u64* slots;
    u64 count;
    u64* Home(ir::Inst* inst) const {
        const auto index = u64(inst->Id()) * 2;
        if (index >= count || count - index < 2) throw std::runtime_error("invalid memory helper home");
        return slots + index;
    }
    static u64 Mask(ir::ValueType type) {
        const auto bits = u32(ir::GetValueSizeByte(type)) * 8;
        return bits >= 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    }
    static bool VectorType(ir::ValueType type) {
        return type >= ir::ValueType::V8 && type <= ir::ValueType::V256;
    }
    u64 Scalar(ir::Value value) const {
        auto raw = Home(value.Def())[0];
        const auto view = value.Type(), producer = value.Def()->ReturnType();
        if (view >= ir::ValueType::U8 && view <= ir::ValueType::S64) {
            raw &= Mask(view);
            if (producer >= ir::ValueType::U8 && producer <= ir::ValueType::S64) raw &= Mask(producer);
        }
        return raw;
    }
    U128 Vector(ir::Value value) const {
        U128 result;
        std::memcpy(&result, Home(value.Def()), sizeof(result));
        return result;
    }
    void Scalar(ir::Inst* inst, u64 value) const {
        if (inst->ReturnType() == ir::ValueType::VOID) return;
        auto* home = Home(inst);
        home[0] = VectorType(inst->ReturnType()) ? value : value & Mask(inst->ReturnType());
        home[1] = 0;
    }
    void Vector(ir::Inst* inst, U128 value) const {
        std::memcpy(Home(inst), &value, sizeof(value));
    }
    u64 Data(const ir::DataClass& data) const {
        return data.IsValue() ? Scalar(data.value) : data.IsImm() ? data.imm.Get() : 0;
    }
    u64 Operand(const ir::Operand& operand) const {
        const auto left = operand.GetLeft(), right = operand.GetRight();
        const auto value = Data(left), other = Data(right);
        if (right.Null()) return value;
        switch (operand.GetOp().type) {
            case ir::OperandOp::Plus: return value + other;
            case ir::OperandOp::LSL: return value << (other & 63);
            case ir::OperandOp::LSR:
                return (value & Mask(left.IsValue() ? left.value.Type() : ir::ValueType::U64)) >> (other & 63);
            case ir::OperandOp::PlusExt: return value + (other << (operand.GetOp().shift_ext & 63));
            default: return value;
        }
    }
    u64 Lambda(const ir::Lambda& lambda) const {
        return lambda.IsValue() ? Scalar(lambda.GetValue()) : lambda.GetImm().Get();
    }
};

template<class T>
T AtomicReplacement(O op, ir::AtomicRMWOp rmw, T old, T operand, u64 carry) {
    if (op == O::AtomicExchange) return operand;
    if (op == O::AtomicFetchAdd) return T(old + operand);
    switch (rmw) {
        case ir::AtomicRMWOp::Add: return T(old + operand);
        case ir::AtomicRMWOp::Sub: return T(old - operand);
        case ir::AtomicRMWOp::And: return T(old & operand);
        case ir::AtomicRMWOp::Or: return T(old | operand);
        case ir::AtomicRMWOp::Xor: return T(old ^ operand);
        case ir::AtomicRMWOp::Neg: return T(T{0} - old);
        case ir::AtomicRMWOp::AddCarry: return T(old + operand + T(carry));
        case ir::AtomicRMWOp::SubBorrow: return T(old - operand - T(carry));
        case ir::AtomicRMWOp::SignedMax: case ir::AtomicRMWOp::SignedMin:
        case ir::AtomicRMWOp::UnsignedMax: case ir::AtomicRMWOp::UnsignedMin:
            return ir::AtomicExtremum(rmw, old, operand);
    }
    return old;
}

template<class T>
T AtomicKernel(State* state, u64 host, O op, ir::AtomicRMWOp rmw,
               u64 operand, u64 desired, u64 carry) {
    auto* pointer = reinterpret_cast<void*>(host);
    const T rhs = T(operand);
    T old;
    // Baseline LoongArch byte/halfword atomics use a containing LL.W word.
    // Keep their precise validated extent by using the shared writer kernel,
    // rather than authorizing neighboring bytes for that wider host access.
    if (sizeof(T) >= 4 && (host & (std::atomic_ref<T>::required_alignment - 1)) == 0) {
        riscv64::MemoryKernelGuard guard{unaligned_atomic_lock};
        SetGuestFaultScope(state, {host, sizeof(T)}, GuestFaultKind::TypedKernel);
        std::atomic_ref<T> ref{*static_cast<T*>(pointer)};
        if (op == O::CompareAndSwap) {
            old = rhs;
            ref.compare_exchange_strong(old, T(desired), std::memory_order_seq_cst);
        } else if (op == O::AtomicExchange) old = ref.exchange(rhs, std::memory_order_seq_cst);
        else if (op == O::AtomicFetchAdd) old = ref.fetch_add(rhs, std::memory_order_seq_cst);
        else {
            old = ref.load(std::memory_order_seq_cst);
            while (!ref.compare_exchange_weak(old, AtomicReplacement(op, rmw, old, rhs, carry),
                                             std::memory_order_seq_cst)) {}
        }
        ClearGuestFaultScope(state);
    } else {
        UnalignedAtomicGuard guard;
        SetGuestFaultScope(state, {host, sizeof(T)}, GuestFaultKind::TypedKernel);
        std::memcpy(&old, pointer, sizeof(T));
        if (op != O::CompareAndSwap || old == rhs) {
            const auto replacement = op == O::CompareAndSwap ? T(desired) :
                    AtomicReplacement(op, rmw, old, rhs, carry);
            std::memcpy(pointer, &replacement, sizeof(T));
        }
        ClearGuestFaultScope(state);
    }
    return old;
}

// Interpreter RunMemoryXX performs another external range-oracle callback.
// These small, explicitly audited kernels preflight once and then authorize
// only their actual guest memory operation. They never replace State callbacks
// or leave a stack-local callback context behind after signal recovery.
void ExecuteGuestMemory(State* state, ir::Inst* inst, KernelValues values) {
    const auto op = inst->GetOp();
    if (op == O::MemoryCopy || op == O::MemoryCopyTSO) {
        const auto size = inst->GetArg<ir::Imm>(2).Get();
        if (!size) return;
        const auto destination_guest = values.Lambda(inst->GetArg<ir::Lambda>(0));
        const auto source_guest = values.Lambda(inst->GetArg<ir::Lambda>(1));
        const auto destination = GuestAddress(state, destination_guest, size);
        ClearGuestFaultScope(state);
        if (state->halt_reason != HaltReason::None) return;
        const auto source = GuestAddress(state, source_guest, size);
        ClearGuestFaultScope(state);
        if (state->halt_reason != HaltReason::None) return;
        riscv64::MemoryKernelGuard guard{unaligned_atomic_lock};
        if (op == O::MemoryCopyTSO) std::atomic_thread_fence(std::memory_order_seq_cst);
        SetGuestFaultScope(state, {destination, size}, GuestFaultKind::TypedKernel, {source, size});
        std::memmove(reinterpret_cast<void*>(destination), reinterpret_cast<const void*>(source), size);
        ClearGuestFaultScope(state);
        if (op == O::MemoryCopyTSO) std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    }
    if (op == O::LoadMemory || op == O::LoadMemoryTSO || op == O::StoreMemory || op == O::StoreMemoryTSO) {
        const bool store = op == O::StoreMemory || op == O::StoreMemoryTSO;
        const bool ordered = op == O::LoadMemoryTSO || op == O::StoreMemoryTSO;
        const auto value = store ? inst->GetArg<ir::Value>(1) : ir::Value{inst};
        const auto size = ir::GetValueSizeByte(value.Type());
        if (!size || size > sizeof(U128)) { state->halt_reason = HaltReason::IllegalCode; return; }
        U128 payload = store ? KernelValues::VectorType(value.Type()) ? values.Vector(value) :
                U128{values.Scalar(value)} : U128{};
        const auto host = GuestAddress(state, values.Operand(inst->GetArg<ir::Operand>(0)), size);
        ClearGuestFaultScope(state);
        if (state->halt_reason != HaltReason::None) return;
        {
            riscv64::MemoryKernelGuard guard{unaligned_atomic_lock};
            if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
            SetGuestFaultScope(state, {host, size}, GuestFaultKind::TypedKernel);
            if (store) std::memcpy(reinterpret_cast<void*>(host), &payload, size);
            else std::memcpy(&payload, reinterpret_cast<const void*>(host), size);
            ClearGuestFaultScope(state);
            if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        if (!store) {
            if (KernelValues::VectorType(value.Type())) values.Vector(inst, payload);
            else values.Scalar(inst, u64(payload));
        }
        return;
    }
    if (op == O::CompareAndSwap128) {
        const auto address = values.Scalar(inst->GetArg<ir::Value>(0));
        const U128 expected = U128{values.Scalar(inst->GetArg<ir::Value>(1))} |
                (U128{values.Scalar(inst->GetArg<ir::Value>(2))} << 64);
        const U128 desired = U128{values.Scalar(inst->GetArg<ir::Value>(3))} |
                (U128{values.Scalar(inst->GetArg<ir::Value>(4))} << 64);
        const auto host = GuestAddress(state, address, sizeof(U128));
        ClearGuestFaultScope(state);
        if (state->halt_reason != HaltReason::None) return;
        U128 old;
        {
            UnalignedAtomicGuard guard;
            SetGuestFaultScope(state, {host, sizeof(U128)}, GuestFaultKind::TypedKernel);
            std::memcpy(&old, reinterpret_cast<const void*>(host), sizeof(old));
            if (old == expected) std::memcpy(reinterpret_cast<void*>(host), &desired, sizeof(desired));
            ClearGuestFaultScope(state);
        }
        values.Vector(inst, old);
        return;
    }
    const bool rmw = op == O::AtomicRMW;
    if (!rmw && op != O::CompareAndSwap && op != O::AtomicExchange && op != O::AtomicFetchAdd)
        throw std::runtime_error("invalid typed guest memory kernel");
    const auto input = inst->GetArg<ir::Value>(rmw ? 2 : 1);
    const auto size = ir::GetValueSizeByte(input.Type());
    if (size != 1 && size != 2 && size != 4 && size != 8)
        throw std::runtime_error("invalid typed atomic width");
    const auto operation = rmw ? inst->GetArg<ir::Imm>(0).Get() : u64{};
    if (operation > u64(ir::AtomicRMWOp::UnsignedMin))
        throw std::runtime_error("invalid typed atomic operation");
    const auto address = values.Scalar(inst->GetArg<ir::Value>(rmw ? 1 : 0));
    const auto operand = values.Scalar(input);
    const auto desired = op == O::CompareAndSwap ? values.Scalar(inst->GetArg<ir::Value>(2)) : 0;
    const auto carry = rmw ? values.Scalar(inst->GetArg<ir::Value>(3)) & 1 : 0;
    const auto host = GuestAddress(state, address, size);
    ClearGuestFaultScope(state);
    if (state->halt_reason != HaltReason::None) return;
    u64 old;
    const auto atomic = static_cast<ir::AtomicRMWOp>(operation);
    switch (size) {
        case 1: old = AtomicKernel<u8>(state, host, op, atomic, operand, desired, carry); break;
        case 2: old = AtomicKernel<u16>(state, host, op, atomic, operand, desired, carry); break;
        case 4: old = AtomicKernel<u32>(state, host, op, atomic, operand, desired, carry); break;
        default: old = AtomicKernel<u64>(state, host, op, atomic, operand, desired, carry); break;
    }
    values.Scalar(inst, old);
}

// Each explicit fallback calls its typed semantic implementation. Native
// emitters never enter the interpreter's opcode dispatcher.
template <void (interp::Interpreter::*Run)(ir::Inst*, interp::InterpStack&)>
void Execute(State* state, ir::Block* block, ir::Inst* inst, u64* slots, u64 count) noexcept {
    ClearGuestFaultScope(state);
    struct ResetScope {
        State* state;
        ~ResetScope() { ClearGuestFaultScope(state); }
    } scope{state};
    try {
        constexpr bool floating_semantics =
            Run == &interp::Interpreter::RunVecFAddScalar32 || Run == &interp::Interpreter::RunVecFSubScalar32 ||
            Run == &interp::Interpreter::RunVecFMulScalar32 || Run == &interp::Interpreter::RunVecFDivScalar32 ||
            Run == &interp::Interpreter::RunVecFAddScalar64 || Run == &interp::Interpreter::RunVecFSubScalar64 ||
            Run == &interp::Interpreter::RunVecFMulScalar64 || Run == &interp::Interpreter::RunVecFDivScalar64 ||
            Run == &interp::Interpreter::RunVecFAdd || Run == &interp::Interpreter::RunVecFSub ||
            Run == &interp::Interpreter::RunVecFMul || Run == &interp::Interpreter::RunVecFDiv ||
            Run == &interp::Interpreter::RunVecFMinMax || Run == &interp::Interpreter::RunVecFUnary ||
            Run == &interp::Interpreter::RunVecFCmp || Run == &interp::Interpreter::RunVecFCmpMask ||
            Run == &interp::Interpreter::RunVecFCvtIntToFloat || Run == &interp::Interpreter::RunVecFCvtFloatToInt ||
            Run == &interp::Interpreter::RunVecFCvtScalar || Run == &interp::Interpreter::RunVecFCvtPacked ||
            Run == &interp::Interpreter::RunVecFMulAdd || Run == &interp::Interpreter::RunVecFRoundInt;
        struct FPEnvironment {
            std::fenv_t saved{};
            bool restore{}, valid{true};
            explicit FPEnvironment(bool enabled) {
                if (!enabled) return;
                restore = std::feholdexcept(&saved) == 0;
                valid = restore && std::fesetround(FE_TONEAREST) == 0;
            }
            ~FPEnvironment() { if (restore) (void)std::fesetenv(&saved); }
        } environment{floating_semantics};
        // Portable FP kernels implement the IR's nearest-even arithmetic.
        // External callbacks keep the caller's FP mode, while kernels neither
        // inherit enabled traps nor leak exception flags into the host.
        if (!environment.valid) { state->halt_reason = HaltReason::IllegalCode; return; }
        constexpr bool memory_semantics =
            Run == &interp::Interpreter::RunLoadMemory || Run == &interp::Interpreter::RunLoadMemoryTSO ||
            Run == &interp::Interpreter::RunStoreMemory || Run == &interp::Interpreter::RunStoreMemoryTSO ||
            Run == &interp::Interpreter::RunMemoryCopy || Run == &interp::Interpreter::RunMemoryCopyTSO ||
            Run == &interp::Interpreter::RunCompareAndSwap || Run == &interp::Interpreter::RunCompareAndSwap128 ||
            Run == &interp::Interpreter::RunAtomicExchange || Run == &interp::Interpreter::RunAtomicFetchAdd ||
            Run == &interp::Interpreter::RunAtomicRMW;
        if (state->halt_reason != HaltReason::None) return;
        if constexpr (memory_semantics) ExecuteGuestMemory(state, inst, {slots, count});
        else {
            interp::InterpStack stack{slots, static_cast<size_t>(count)};
            interp::Interpreter interpreter{*state, block};
            (interpreter.*Run)(inst, stack);
        }
    } catch (...) { state->halt_reason = HaltReason::IllegalCode; }
}

constexpr SemanticHelper helpers[] = {
    nullptr,  // OpCode::Void precedes the ir.inc-generated opcodes.
#define INST(name, ...) &Execute<&interp::Interpreter::Run##name>,
#include "runtime/ir/ir.inc"
#undef INST
};
static_assert(std::size(helpers) == size_t(O::BASE_COUNT));
}  // namespace

void JitTranslator::Translate(ir::Block* input) {
    if (execution_mode == ExecutionMode::SingleInstruction &&
        (!SingleInstructionControlFlowValid(*input) || !function_exits.empty() ||
         !function_halt_checks.empty() || WidePlan::Needed(input)))
        throw std::invalid_argument("LoongArch Step requires a fresh finite 128-bit instruction unit");
    for (auto& inst : input->GetInstList()) {
        if (inst.GetOp() == O::VecUnsignedEstimate) (void)ValidateUnsignedEstimate(&inst);
        if (inst.GetOp() == O::VecSatAdd || inst.GetOp() == O::VecSatSub) (void)ValidateSaturatingAddSub(&inst, 32);
    }
    const bool has_markers = std::any_of(input->GetInstList().begin(), input->GetInstList().end(),
        [](const ir::Inst& inst) { return inst.GetOp() == O::AdvancePC; });
    if (has_markers || WidePlan::Needed(input) || context.UsesSparseHomes() || context.UsesUniformForwarding()) {
        auto wide = WidePlan::Build(input);
        context.RemapFlow(wide.instructions);
        decltype(function_exits) exits;
        for (const auto& [inst, terminal] : function_exits)
            exits.emplace(wide.instructions.at(inst), wide.RemapTerminal(terminal));
        function_exits = std::move(exits);
        decltype(function_halt_checks) checks;
        for (auto* inst : function_halt_checks) checks.insert(wide.instructions.at(inst));
        function_halt_checks = std::move(checks);
        memory_extents = std::move(wide.memory_extents); memory_continuations = std::move(wide.memory_continuations);
        wide_pairs = std::move(wide.native_pairs);
        wide_ir = std::move(wide.body); input = wide_ir.get();
    }
    block = input;
    // Direct translator users also need retirement polls. Keep source IR
    // untouched, and retain production snapshots already inserted by Prepare.
    bool inserted_checkpoint{};
    for (auto it = block->GetInstList().begin(); it != block->GetInstList().end(); ++it) {
        if (it->GetOp() != O::AdvancePC) continue;
        auto next = std::next(it);
        if (next != block->GetInstList().end() && next->GetOp() == O::Checkpoint) continue;
        auto* checkpoint = new ir::Inst(O::Checkpoint);
        checkpoint->SetArg(0, ir::Params{});
        block->InsertAfter(checkpoint, &*it);
        inserted_checkpoint = true;
    }
    if (inserted_checkpoint) block->ReIdInstr();
    guest_instructions = std::any_of(block->GetInstList().begin(), block->GetInstList().end(),
            [](const ir::Inst& inst) { return inst.GetOp() == O::AdvancePC; });
    context.ConfigureBlock(block);
    for (auto& inst : block->GetInstList()) if (inst.GetOp() == O::BindLabel)
        if (!labels.try_emplace(inst.GetArg<ir::Value>(0).Def()).second)
            throw std::runtime_error("LoongArch branch has multiple bound targets");
    for (auto& inst : block->GetInstList())
        if ((inst.GetOp() == O::Goto || inst.GetOp() == O::NotGoto) && !labels.contains(context.BranchTarget(&inst)))
            throw std::runtime_error("LoongArch branch target is not bound");
    ConfigurePhis();
    context.ConfigureUniforms(block, function_exits);
    context.ConfigureRegisters(block, function_exits);
    context.ConfigureHomes(block, function_exits, wide_pairs);
    context.ConfigureFlags(block, function_exits, function_halt_checks);
    vectors.Configure(block, wide_pairs, context.VectorAllocationEnabled(),
                      context.FlowTargets(), context.FlowStops(), function_exits,
                      context.VectorCFGAllocationEnabled());
    EmitPrologue();
    EmitPhiMoves(entry_phis);
    if (execution_mode == ExecutionMode::Ordinary) { Poll(); CheckHalt(); }
    for (auto& inst : block->GetInstList()) {
        current_inst = &inst;
        if (auto moves = fallthrough_phis.find(&inst); moves != fallthrough_phis.end()) {
            context.FlushValues(); context.DiscardValues(); EmitPhiMoves(moves->second);
        }
        context.BeginInstruction(&inst);
        vectors.BeginInstruction(&inst);
        if (SparseHomesPlan::MayCallTypedHelper(&inst) || AuditedGuestMemoryOpcode(inst.GetOp()) ||
            inst.GetOp() == O::CheckMemoryAlignment)
            PublishPC();
        if (auto exit = function_exits.find(&inst); exit != function_exits.end()) {
            context.FlushValues(); context.DiscardValues(); EmitTerminal(exit->second);
        } else if (function_halt_checks.contains(&inst)) {
            PublishPC(); vectors.Barrier(); CheckHalt();
        }
        const auto before = context.CurrentBufferSize();
        const auto wide_before = vectors.Stats().lasx_operations;
        if (!vectors.EmitWide(&inst)) Emit(&inst);
        else stats.direct += vectors.Stats().lasx_operations - wide_before;
        context.FinishInstruction();
        vectors.FinishInstruction();
        stats.bytes[size_t(inst.GetOp())] += context.CurrentBufferSize() - before;
    }
    context.FlushValues(); context.DiscardValues();
    vectors.Barrier();
    EmitTerminal(block->GetTerminal());
    if (execution_mode == ExecutionMode::SingleInstruction) {
        context.GetMasm().Bind(&single_retirement);
        EmitSingleRetirement();
    }
    recovery_offset = context.CurrentBufferSize();
    auto& as = context.GetMasm();
    as.Bind(&fault_recovery);
    context.DiscardValues();
    context.ReloadFlags();
    context.Load(values, frame, 160);
    context.Load(a0, state, state_offset_halt_reason, 4);
    as.J(&epilogue);
    as.Bind(&epilogue);
    EmitEpilogue();
}

void JitTranslator::Emit(ir::Inst* inst) {
    ++stats.direct;
    switch (inst->GetOp()) {
#define INST(name, ...) case O::name: Emit##name(inst); break;
#include "runtime/ir/ir.inc"
#undef INST
        default: throw std::runtime_error("invalid LoongArch IR opcode");
    }
}

void JitTranslator::Call(GPR target) { context.GetMasm().jirl(ra, target, 0); }

void JitTranslator::EmitHelper(ir::Inst* inst) {
    --stats.direct; ++stats.helpers;
    vectors.Barrier();
    if (execution_mode == ExecutionMode::SingleInstruction && AuditedGuestMemoryOpcode(inst->GetOp()))
        typed_guest_accesses = true;
    auto& as = context.GetMasm();
    context.FlushValues(); context.DiscardValues(); context.PublishFlags();
    context.Store(zero, state, state_offset_loongarch_guest_fault_cookie);
    as.dbar(0);
    context.PublishInputs(inst);
    // Typed semantics still use Id*16. Validate every potential home before
    // embedding its IR pointer; register-only values never invent slot zero.
    (void)context.HomeOffset(inst);
    for (u32 i = 0; i < ir::Inst::max_args; ++i) {
        auto& arg = inst->ArgAt(i);
        if (arg.IsValue()) (void)context.HomeOffset(arg.Get<ir::Value>().Def());
        else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue())
            (void)context.HomeOffset(arg.Get<ir::Lambda>().GetValue().Def());
        else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>())
            if (param.data.IsValue()) (void)context.HomeOffset(param.data.value.Def());
    }
    if (!inst->IsPseudoOperation())
        for (auto* pseudo : inst->GetPseudoOperations()) (void)context.HomeOffset(pseudo);
    // An atomic helper must acquire the writer gate without holding a native
    // reader lease. Ordinary memory kernels participate in the same protocol.
    as.MV(a0, state); context.HostAddress(t0, reinterpret_cast<u64>(&ReleaseMemory)); Call(t0);
    as.MV(a0, state); context.HostAddress(a1, reinterpret_cast<u64>(block));
    context.HostAddress(a2, reinterpret_cast<u64>(inst)); as.MV(a3, values);
    as.LI(a4, context.SlotCount() * 2);
    context.HostAddress(t0, reinterpret_cast<u64>(helpers[size_t(inst->GetOp())])); Call(t0);
    context.ReloadFlags(); CheckHalt();
    context.ReloadDefinitions(inst);
}

void JitTranslator::EmitPrologue() {
    auto& as = context.GetMasm();
    stats.frame_size = kBlockSavedFrameSize;
    stats.saved_gprs = saved_gprs.size(); stats.saved_fprs = saved_fprs.size();
    as.addi_d(sp, sp, -s32(kBlockSavedFrameSize));
    for (u32 i = 0; i < saved_gprs.size(); ++i) as.st_d(saved_gprs[i], sp, kGPRSaveOffset + i * 8);
    for (u32 i = 0; i < saved_fprs.size(); ++i) as.fst_d(saved_fprs[i], sp, kFPRSaveOffset + i * 8);
    as.movfcsr2gr(t0, fcsr0); as.st_d(t0, sp, kFCSRSaveOffset);
    as.MV(frame, sp); as.MV(state, a0);
    if (execution_mode == ExecutionMode::SingleInstruction) {
        Label enter;
        as.MV(a0, state); context.HostAddress(t0, reinterpret_cast<u64>(&ObserveSingleEntry)); Call(t0);
        as.BEQ(a0, zero, &enter);
        EmitRestoreFrame();
        as.Bind(&enter);
        as.LI(t0, block->GetStartLocation().Value());
        context.Store(t0, state, state_offset_current_loc);
        context.Store(t0, state, state_offset_prev_loc);
    }
    context.Store(zero, state, state_offset_loongarch_guest_fault_cookie);
    as.dbar(0);
    as.LI(t0, u64(context.SlotCount()) * kValueStride); as.sub_d(sp, sp, t0); as.MV(values, sp);
    as.st_d(values, frame, 160);
    // The high half of a scalar home is defined as zero for typed semantic
    // helpers and vector bitcasts, including loop-carried values.
    Label initialize, initialized;
    as.MV(t1, values); as.MV(t2, t0);
    as.Bind(&initialize); as.BEQ(t2, zero, &initialized);
    as.st_d(zero, t1, 0); as.st_d(zero, t1, 8);
    as.addi_d(t1, t1, 16); as.addi_d(t2, t2, -16); as.J(&initialize);
    as.Bind(&initialized);
    context.ReloadFlags();
    as.LILabel(t0, &fault_recovery);
    context.Store(t0, state, state_offset_riscv_recovery_pc);
    context.Store(frame, state, state_offset_riscv_recovery_frame);
}

void JitTranslator::EmitEpilogue() {
    auto& as = context.GetMasm();
    context.Store(zero, state, state_offset_loongarch_guest_fault_cookie);
    as.dbar(0);
    // Poll, explicit returns and fault recovery join here. Their incoming
    // dirty states need not match the last lexical emitter/recovery reload.
    context.PublishFlags(true);
    as.st_d(a0, frame, 168);
    as.MV(a0, state); context.HostAddress(t0, reinterpret_cast<u64>(&ReleaseMemory)); Call(t0);
    context.Store(zero, state, state_offset_riscv_recovery_pc);
    context.Store(zero, state, state_offset_riscv_recovery_frame);
    as.MV(sp, frame); as.ld_d(a0, sp, 168);
    EmitRestoreFrame();
}

void JitTranslator::EmitRestoreFrame() {
    auto& as = context.GetMasm();
    as.ld_d(t0, sp, kFCSRSaveOffset); as.movgr2fcsr(fcsr0, t0);
    for (u32 i = 0; i < saved_fprs.size(); ++i) as.fld_d(saved_fprs[i], sp, kFPRSaveOffset + i * 8);
    for (u32 i = 0; i < saved_gprs.size(); ++i) as.ld_d(saved_gprs[i], sp, kGPRSaveOffset + i * 8);
    as.addi_d(sp, sp, kBlockSavedFrameSize); as.jirl(zero, ra, 0);
}

void JitTranslator::RecordGuestAccess(u32 begin, u32 size, GuestAccessKind kind,
                                     GPR address, s32 displacement) {
    if (execution_mode != ExecutionMode::SingleInstruction) return;
    const u32 end = context.CurrentBufferSize();
    if (end != begin + 4 || (begin & 3)) {
        guest_access_coverage = GuestAccessCoverage::Unsupported;
        return;
    }
    guest_accesses.push_back({begin, end, size, kind, static_cast<u8>(address.getIdx()), displacement});
}

#define FALLBACK(name) void JitTranslator::Emit##name(ir::Inst* inst) { EmitHelper(inst); }
#include "fallback.inc"
#undef FALLBACK
}  // namespace swift::runtime::backend::loongarch64
