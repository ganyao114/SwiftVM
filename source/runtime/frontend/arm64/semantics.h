#pragma once

#include "aarch64/decoder-aarch64.h"
#include "aarch64/instructions-aarch64.h"
#include "build_ir.h"
#include "instruction.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/exit.h"
#include "runtime/frontend/guest_events.h"
#include "state_service.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <type_traits>

namespace swift::arm64::detail {
using namespace runtime::ir;
using namespace vixl::aarch64;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u128 = unsigned __int128;
using VAddr = u64;
using GuestState = runtime::arm64::GuestState;
using runtime::arm64::Numeric;

struct Rejected {
    BuildError error;
    Error detail{Error::None};
};
enum class InterruptReason { SVC, HLT, BRK, ILL_CODE, PAGE_FATAL, FALLBACK };

constexpr int HighestBit(u32 value) { return int(std::bit_width(value)) - 1; }

constexpr Type GPRType(bool wide) { return wide ? Type::U64 : Type::U32; }

constexpr Type ScalarType(unsigned bits) {
    return bits == 8 ? Type::U8 : bits == 16 ? Type::U16 : bits == 32 ? Type::U32 : Type::U64;
}

constexpr Type StorageType(Type type) {
    return type >= Type::V8 && type <= Type::V64 ? ScalarType(TypeBits(type)) : type;
}

constexpr Type VectorType(unsigned bits) {
    return bits == 8    ? Type::V8
           : bits == 16 ? Type::V16
           : bits == 32 ? Type::V32
           : bits == 64 ? Type::V64
                        : Type::V128;
}

template <class Sink> class Semantics final : public DecoderVisitor {
public:
    using NativeValue = typename Sink::Value;
    using Operand = typename Sink::Operand;

    // A temporary consumer view of a real sink value. It owns no IR node,
    // instruction list, def/use graph, or persistent operand storage.
    struct Value {
        NativeValue raw{};
        Type view{Type::Void};
        OpCode origin{OpCode::Nop};

        Type ViewType() const { return view; }

        Value WithView(Type type) {
            view = type;
            return *this;
        }

        Value SetCastType(Type type) { return WithView(type); }
    };

    struct Imm {
        u64 bits{};
        Type type{Type::U64};

        template <class T> explicit Imm(T value)
                : bits(static_cast<u64>(value))
                , type(sizeof(T) <= 1   ? Type::U8
                       : sizeof(T) <= 2 ? Type::U16
                       : sizeof(T) <= 4 ? Type::U32
                                        : Type::U64) {}

        u64 Get() const { return bits; }

        s64 GetSigned() const { return static_cast<s64>(bits); }
    };

    struct Uniform {
        u32 offset;
        Type type;
    };

    struct Input {
        Value value;
        Imm imm{u64{0}};
        bool immediate{};

        Input(Value v) : value(v) {}

        Input(Imm i) : imm(i), immediate(true) {}

        bool IsImm() const { return immediate; }

        bool IsValue() const { return !immediate; }

        Value GetValue() const { return value; }

        Imm GetImm() const { return imm; }
    };

    struct Control {
        enum class Kind { None, Jump, Conditional, Dispatch, Service };
        Kind kind{};
        Input destination{Imm{u64{0}}};
        Value condition;
        u64 yes{}, no{}, service{};
    };

    Semantics(Sink& sink, StateLayout layout, TargetInfo target)
            : sink(sink), layout(layout), target(target) {}

    void Execute(const Instruction& instruction, bool budget_safepoint = false) {
        current_pc = instruction.PC();
        current_word = instruction.Word();
        end_decode_ = false;
        control = {};
        if (target.Has(TargetFeature::GuestEvents)) {
            using runtime::frontend::CrossesGuestEventWindow;
            using runtime::frontend::GuestEventTransfer;
            bool observe = budget_safepoint || CrossesGuestEventWindow(current_pc, NextPC());
            bool dispatch_observe{};
            const auto flow = instruction.Flow();
            if (flow == FlowKind::Return)
                dispatch_observe = true;
            else if (flow == FlowKind::Jump || flow == FlowKind::Call ||
                     flow == FlowKind::Conditional) {
                u64 destination{};
                if (instruction.RelativeTarget(destination))
                    observe |= GuestEventTransfer(current_pc, destination);
                else
                    dispatch_observe = true;
            }
            if (observe)
                Emit<OpCode::GuestSafepoint>(Imm{current_pc});
            else if (dispatch_observe)
                Emit<OpCode::GuestDispatchSafepoint>(Imm{current_pc});
        }
        using Handler = void (Semantics::*)(const vixl::aarch64::Instruction*);
        static constexpr std::array<Handler, static_cast<std::size_t>(Family::Count)> handlers{
#define ARM_FAMILY(name) static_cast<Handler>(&Semantics::Visit##name),
#include "instruction_families.inc"
#undef ARM_FAMILY
        };
        if (instruction.FamilyCode() >= handlers.size()) throw Rejected{BuildError::InvalidInput};
        alignas(4) u32 bits = current_word;
        (this->*handlers[instruction.FamilyCode()])(
                reinterpret_cast<const vixl::aarch64::Instruction*>(&bits));
    }

    Control control;
    u64 current_pc{};
    bool end_decode_{};

private:
    Sink& sink;
    StateLayout layout;
    TargetInfo target;

    enum class AddrMode { Offset, PreIndex, PostIndex };

    static u32 VRegOff(u8 code) { return 272 + u32(code) * 16; }

    static u32 GPROffset(u8 code) { return u32(code) * 8; }

    static unsigned ValueBytes(Type type) { return TypeBits(type) / 8; }

    static Type VectorBytesType(unsigned bytes) { return VectorType(bytes * 8); }

    u64 NextPC() const { return current_pc + 4; }

    u64 CurrentPC() const { return current_pc; }

    static Input SingleOperand(Input input) { return input; }

    u32 MapOffset(u32 offset) const {
        if (offset < 256) return layout.gprs_offset + offset;
        if (offset < 264) return layout.pc_offset + offset - 256;
        if (offset < 272) return layout.flags_offset + offset - 264;
        if (offset < 784) return layout.fprs_offset + offset - 272;
        if (offset < 788) return layout.fpcr_offset + offset - 784;
        if (offset < 792) return layout.fpsr_offset + offset - 788;
        if (offset < 800) return layout.tpidr_offset + offset - 792;
        if (offset < 804) return layout.numeric_control_offset + offset - 800;
        if (offset < 808) return layout.helper_status_offset + offset - 804;
        if (offset < 812) return layout.service_word_offset + offset - 808;
        return layout.reservation_valid_offset + offset - 812;
    }

    Operand Arg(Value v) const { return Operand::ValueOf(v.raw, v.view); }

    Operand Arg(Imm i) const { return Operand::Immediate(i.bits, i.type); }

    Operand Arg(Uniform u) const { return Operand::Uniform(MapOffset(u.offset), u.type); }

    Operand Arg(Input i) const { return i.immediate ? Arg(i.imm) : Arg(i.value); }

    Operand Arg(Flag flag) const { return Operand::Flags(flag); }

    Operand Arg(runtime::ir::Condition cond) const { return Operand::Condition(cond); }

    static Type Kind(Value v) { return v.view; }

    static Type Kind(Imm i) { return i.type; }

    static Type Kind(Uniform u) { return u.type; }

    static Type Kind(Input i) { return i.immediate ? i.imm.type : i.value.view; }

    static Type Kind(Flag) { return Type::Void; }

    static Type Kind(runtime::ir::Condition) { return Type::Void; }

    template <OpCode op, Type forced = Type::Void, class... Args> Value Emit(Args... input) {
        const std::array<Operand, sizeof...(Args)> args{Arg(input)...};
        const std::array<Type, sizeof...(Args)> types{Kind(input)...};
        Type type = forced;
        if constexpr (schemas[static_cast<std::size_t>(op)].result == ResultPolicy::Void)
            type = Type::Void;
        else if constexpr (schemas[static_cast<std::size_t>(op)].result == ResultPolicy::Fixed ||
                           schemas[static_cast<std::size_t>(op)].result ==
                                   ResultPolicy::IntegerFixed)
            type = schemas[static_cast<std::size_t>(op)].fixed_type;
        else if (type == Type::Void) {
            if constexpr (op == OpCode::VecSharedZero || op == OpCode::VecLoadConst ||
                          op == OpCode::VecDup64 || op == OpCode::CompareAndSwap128)
                type = Type::V128;
            else if constexpr (op == OpCode::GetUniformAddress || op == OpCode::VecExtract64 ||
                               op == OpCode::GetFlags || op == OpCode::ZeroExtend64 ||
                               op == OpCode::CountLeadingZeros64)
                type = Type::U64;
            else if constexpr (op == OpCode::CountLeadingZeros32)
                type = Type::U32;
            else if constexpr (op == OpCode::TestZero || op == OpCode::TestNotZero ||
                               op == OpCode::TestBit || op == OpCode::TestFlags ||
                               op == OpCode::TestNotFlags)
                type = Type::Bool;
            else if constexpr (op == OpCode::Select || op == OpCode::CondSelect)
                type = types[1];
            else if constexpr (sizeof...(Args))
                type = types[0];
        }
        const auto result = sink.template Emit<op>(type, std::span<const Operand>{args});
        if (!result) throw Rejected{BuildError::IRRejected, result.error};
        if constexpr (op == OpCode::StoreMemory || op == OpCode::StoreMemoryTSO) ClearReservation();
        return {result.GetValue(), type, op};
    }

#define ARM_OP(name)                                                                               \
    template <Type type = Type::Void, class... A> Value name(A... a) {                             \
        return Emit<OpCode::name, type>(a...);                                                     \
    }

    ARM_OP(LoadImm)
    ARM_OP(LoadUniform)
    ARM_OP(StoreUniform)
    ARM_OP(GetUniformAddress)
    ARM_OP(Add)
    ARM_OP(Sub)
    ARM_OP(Adc)
    ARM_OP(Mul)
    ARM_OP(And)
    ARM_OP(Or)
    ARM_OP(Xor)
    ARM_OP(AndNot)
    ARM_OP(CondSet)
    ARM_OP(LslImm)
    ARM_OP(LsrImm)
    ARM_OP(AsrImm)
    ARM_OP(RorImm)
    ARM_OP(LslValue)
    ARM_OP(LsrValue)
    ARM_OP(AsrValue)
    ARM_OP(RorValue)
    ARM_OP(ZeroExtend64)
    ARM_OP(CountLeadingZeros32)
    ARM_OP(CountLeadingZeros64)
    ARM_OP(BitInsert)
    ARM_OP(TestZero)
    ARM_OP(TestNotZero)
    ARM_OP(TestBit)
    ARM_OP(TestFlags)
    ARM_OP(TestNotFlags)
    ARM_OP(Select)
    ARM_OP(SetCarry)
    ARM_OP(SetOverflow)
    ARM_OP(GetFlags)
    ARM_OP(MemoryBarrierTSO)
    ARM_OP(CheckMemoryAlignment)
    ARM_OP(StoreMemory)
    ARM_OP(StoreMemoryTSO)
    ARM_OP(VecAnd)
    ARM_OP(VecAndNot)
    ARM_OP(VecOr)
    ARM_OP(VecXor)
    ARM_OP(VecAdd)
    ARM_OP(VecSub)
    ARM_OP(VecMul)
    ARM_OP(VecCmpEq)
    ARM_OP(VecCmpGt)
    ARM_OP(VecMax)
    ARM_OP(VecMin)
    ARM_OP(VecZip)
    ARM_OP(VecUnzip)
    ARM_OP(VecShiftLeftImm)
    ARM_OP(VecShiftRightImm)
    ARM_OP(VecShiftRightArithmeticImm)
    ARM_OP(VecLoadConst)
    ARM_OP(VecSharedZero)
    ARM_OP(VecDup64)
    ARM_OP(VecExtract64)
    ARM_OP(VecExtractBytes)
    ARM_OP(VecShuffle32)
    ARM_OP(VecShuffle32TwoSrc)
    ARM_OP(VecTableLookup8)
    ARM_OP(VecPclMul)
    ARM_OP(VecAesEnc)
    ARM_OP(VecAesEncLast)
    ARM_OP(VecAesDec)
    ARM_OP(VecAesDecLast)
    ARM_OP(VecSha256Msg1)
    ARM_OP(VecSha256Msg2)
    ARM_OP(VecSha256Rnds2)
    ARM_OP(VecSatAdd)
    ARM_OP(VecSatSub)
    ARM_OP(Crc32c)
#undef ARM_OP

    Value ImmValue(u64 bits, Type type) { return Emit<OpCode::LoadImm>(Imm{bits}).WithView(type); }

    Value TypedLoad(Type type, Input address, bool ordered = false) {
        type = StorageType(type);
        const auto out = ordered ? sink.template Emit<OpCode::LoadMemoryTSO>(type, {Arg(address)})
                                 : sink.template Emit<OpCode::LoadMemory>(type, {Arg(address)});
        if (!out) throw Rejected{BuildError::IRRejected, out.error};
        return {out.GetValue(), type, OpCode::LoadMemory};
    }

    template <OpCode op, class... Args> Value TypedAtomic(Type type, Args... args) {
        const std::array<Operand, sizeof...(Args)> a{Arg(args)...};
        const auto r = sink.template Emit<op>(type, std::span<const Operand>{a});
        if (!r) throw Rejected{BuildError::IRRejected, r.error};
        ClearReservation();
        return {r.GetValue(), type, op};
    }

    Value Reinterpret(Value input, Type type) {
        type = StorageType(type);
        if (TypeBits(input.view) == 128 && TypeBits(type) <= 64)
            return VecExtract64(input, Imm{0u}).WithView(type);
        return input.WithView(type);
    }

    Value TypedLoadTSO(Type type, Input address) { return TypedLoad(type, address, true); }

    Value TypedCAS(Type type, Value a, Value b, Value c) {
        return TypedAtomic<OpCode::CompareAndSwap>(type, a, b, c);
    }

    Value TypedCAS128(Type type, Value a, Value b, Value c, Value d, Value e) {
        return TypedAtomic<OpCode::CompareAndSwap128>(type, a, b, c, d, e);
    }

    Value TypedExchange(Type type, Value a, Value b) {
        return TypedAtomic<OpCode::AtomicExchange>(type, a, b);
    }

    Value TypedFetchAdd(Type type, Value a, Value b) {
        return TypedAtomic<OpCode::AtomicFetchAdd>(type, a, b);
    }

    Value TypedRMW(Type type, Imm op, Value a, Value b, Value c) {
        return TypedAtomic<OpCode::AtomicRMW>(type, op, a, b, c);
    }

    Value CondSelect(runtime::ir::Condition cond, Value a, Value b) {
        return Select(CondPassed(static_cast<vixl::aarch64::Condition>(cond)), a, b);
    }

    Value Crc32(Value, Value, Imm) { return Helper(Numeric::Crc32).WithView(Type::U32); }

    Value ReadXRegister(u8 code, Reg31Mode mode = Reg31IsZeroRegister) {
        return ReadRegister(code, Type::U64, mode);
    }

    Value ReadWRegister(u8 code, Reg31Mode mode = Reg31IsZeroRegister) {
        return ReadRegister(code, Type::U32, mode);
    }

    void WriteSRegister(u8 code, Value value) { WriteVRegister(code, value); }

    void WriteDRegister(u8 code, Value value) { WriteVRegister(code, value); }

    void WriteQRegister(u8 code, Value value) { WriteVRegister(code, value); }

    Value Goto(Value condition) {
        return {sink.LocalGoto(condition.raw, condition.view), condition.view, OpCode::Goto};
    }

    void BindLabel(Value label) { sink.LocalBind(label.raw); }

    void SaveFlags(Value value, Flag flags) {
        auto producer = sink.ProducerOf(value.raw);
        auto metadata = sink.Metadata(producer);
        metadata.flag_outputs |= FlagBits(flags);
        metadata.profile = (value.origin == OpCode::And || value.origin == OpCode::Or ||
                            value.origin == OpCode::Xor || value.origin == OpCode::AndNot)
                                   ? FlagProfile::LogicalZeroCVA
                                   : FlagProfile::ArithmeticCanonicalCarry;
        if (const auto e = sink.SetMetadata(producer, metadata); e != Error::None)
            throw Rejected{BuildError::IRRejected, e};
        if (value.origin == OpCode::Sub && (FlagBits(flags) & FlagBits(Flag::Carry)))
            Emit<OpCode::InvertCarry>();
    }

    template <Type type> Value Div(Value lhs, Input rhs) {
        const auto right = rhs.IsValue() ? rhs.value : ImmValue(rhs.imm.bits, type);
        auto zero = TestZero(right);
        auto safe = Select(zero, ImmValue(1, type), right);
        Value result;
        if (lhs.view == Type::S64 || lhs.view == Type::S32) {
            auto left64 = Emit<OpCode::SignExtend, Type::U64>(lhs),
                 right64 = Emit<OpCode::SignExtend, Type::U64>(safe.WithView(lhs.view));
            auto overflow = BoolAnd(TestZero(Xor(left64, SingleOperand(Imm{UINT64_C(1) << 63}))),
                                    TestZero(Xor(right64, SingleOperand(Imm{UINT64_MAX}))));
            right64 = Select(overflow, ImmValue(1, Type::U64), right64);
            result = Emit<OpCode::SignedDiv64, Type::U64>(left64, right64).WithView(type);
        } else
            result = Emit<OpCode::Div, type>(lhs, safe);
        return Select(zero, ImmValue(0, type), result);
    }

    void WritePC(Input input) { control.destination = input; }

    void SetLocation(Input input) {
        if (target.Has(TargetFeature::FaultLocations) && input.IsImm() &&
            input.imm.bits == current_pc)
            Emit<OpCode::FaultLocation>(input.imm);
        else
            StoreUniform(Uniform{256, Type::U64},
                         input.IsImm() ? ImmValue(input.imm.bits, Type::U64) : input.value);
    }

    void BranchImm(u64 pc) {
        control.kind = Control::Kind::Jump;
        control.yes = pc;
        end_decode_ = true;
    }

    void BranchChoice(Value cond, u64 yes, u64 no) {
        control.kind = Control::Kind::Conditional;
        control.condition = cond;
        control.yes = yes;
        control.no = no;
        end_decode_ = true;
    }

    void ReturnToDispatcher() {
        control.kind = Control::Kind::Dispatch;
        end_decode_ = true;
    }

    void Return() { ReturnToDispatcher(); }

    void PushRSB(Input) {}

    bool IsFunctionMode() const { return true; }

    void Interrupt(InterruptReason reason, u64 resume) {
        u64 service;
        switch (reason) {
            case InterruptReason::SVC:
                service = runtime::ExitValue(runtime::NativeExit::Syscall);
                break;
            case InterruptReason::BRK:
                service = runtime::SemanticExceptionExit(runtime::GuestExceptionKind::Breakpoint);
                break;
            case InterruptReason::ILL_CODE:
                service = runtime::SemanticExceptionExit(
                        runtime::GuestExceptionKind::IllegalInstruction);
                break;
            default:
                throw Rejected{BuildError::Unsupported};
        }
        control.kind = Control::Kind::Service;
        control.yes = resume;
        control.service = service;
        end_decode_ = true;
    }

    void ClearReservation() {
        StoreUniform(Uniform{u32(offsetof(GuestState, reservation_valid)), Type::U32},
                     ImmValue(0, Type::U32));
    }

    void RequestStateService(runtime::arm64::StateServiceRequest request) {
        StoreUniform(Uniform{u32(offsetof(GuestState, service_word)), Type::U32},
                     ImmValue(runtime::arm64::EncodeStateService(request), Type::U32));
        control.kind = Control::Kind::Service;
        control.yes = current_pc;
        control.service = runtime::ExitValue(runtime::NativeExit::StateService);
        end_decode_ = true;
    }

    Value Helper(Numeric family, u32 extra = 0) {
        // Numeric leaves receive one contiguous architectural state, whereas
        // ordinary uniform IR can also use a split frontend layout.
        const u64 base = layout.gprs_offset;
        if (layout.pc_offset != base + offsetof(GuestState, pc) ||
            layout.flags_offset != base + offsetof(GuestState, packed_flags) ||
            layout.fprs_offset != base + offsetof(GuestState, q) ||
            layout.fpcr_offset != base + offsetof(GuestState, fpcr) ||
            layout.fpsr_offset != base + offsetof(GuestState, fpsr) ||
            layout.tpidr_offset != base + offsetof(GuestState, tpidr_el0) ||
            layout.numeric_control_offset != base + offsetof(GuestState, numeric_control) ||
            layout.helper_status_offset != base + offsetof(GuestState, helper_status))
            throw Rejected{BuildError::InvalidInput};
        return {sink.Helper(family, current_word, current_pc, extra, layout.gprs_offset),
                Type::U64,
                OpCode::CallLambda};
    }

    bool TryNumeric(const vixl::aarch64::Instruction* instr, Numeric family) {
        if (!runtime::arm64::SupportsNumeric(family, instr->GetInstructionBits())) return false;
        Helper(family);
        return true;
    }

    u32 current_word{};
#include "semantics_control.inc"
#include "semantics_crypto.inc"
#include "semantics_float.inc"
#include "semantics_lanes.inc"
#include "semantics_memory.inc"
#include "semantics_permutation.inc"
#include "semantics_registers.inc"
#include "semantics_scalar.inc"
#include "semantics_shift.inc"
#include "semantics_vector.inc"
#include "semantics_vector_arithmetic.inc"
};
}  // namespace swift::arm64::detail
