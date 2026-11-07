#include "interpreter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

#include "runtime/arch/x86/flags.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/state.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/arch/x86/bit_operations.h"
#include "runtime/arch/x86/sse42_string.h"
#include "runtime/platform/fp_environment.h"
#include "semantics.h"

namespace swift::x86 {
namespace {
using namespace runtime::ir;
namespace arch = runtime::x86;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u128 = unsigned __int128;
using s128 = __int128;

struct DirectExit {
    StepKind kind;
    u64 address{}, service{};
};

struct Numeric {
    u128 bits{};
    u64 flags{}, extra{};
    Type type{Type::Void};
    bool raw_carry{};
    OperationMetadata metadata;

    explicit operator bool() const noexcept { return type != Type::Void; }
};

struct ConcreteValue {
    Numeric number;

    explicit operator bool() const noexcept { return bool(number); }
};

struct ConcreteOperand {
    OperandKind kind{};
    Type type{Type::Void};
    Numeric value;
    u64 payload{};
    unsigned count{}, code{};

    static ConcreteOperand ValueOf(ConcreteValue value, Type view = Type::Void) {
        return {OperandKind::Value, view, value.number};
    }

    static ConcreteOperand Immediate(u64 value, Type type = Type::U64) {
        return {OperandKind::Immediate, type, {}, value};
    }

    static ConcreteOperand Uniform(u32 offset, Type type) {
        return {OperandKind::Uniform, type, {}, offset};
    }

    static ConcreteOperand Flags(Flag value) {
        return {OperandKind::Flags, Type::Void, {}, FlagBits(value)};
    }

    static ConcreteOperand Flags(u64 value) { return {OperandKind::Flags, Type::Void, {}, value}; }

    static ConcreteOperand Condition(Condition value) {
        return {OperandKind::Condition, Type::Void, {}, static_cast<u64>(value)};
    }

    static ConcreteOperand Expression(OperandOp op, unsigned count, unsigned shift = 0) {
        return {OperandKind::Expression,
                Type::Void,
                {},
                0,
                count,
                static_cast<unsigned>(op) | (shift << 8)};
    }
};

struct ConcreteResult {
    Error error{Error::None};
    ConcreteValue value;

    explicit operator bool() const noexcept { return error == Error::None; }

    ConcreteValue GetValue() const { return value; }
};

constexpr u64 Mask(unsigned bits) { return bits >= 64 ? UINT64_MAX : (u64{1} << bits) - 1; }

constexpr u128 WideMask(unsigned bits) { return bits >= 128 ? ~u128{} : (u128{1} << bits) - 1; }

std::int64_t Signed(u64 bits, unsigned width) {
    if (width == 64) return std::bit_cast<std::int64_t>(bits);
    const auto sign = u64{1} << (width - 1);
    return static_cast<std::int64_t>((bits & Mask(width)) ^ sign) - static_cast<std::int64_t>(sign);
}

u64 SelectedFlags(u64 mask) {
    return ((mask & FlagBits(Flag::Carry)) ? arch::flags::Carry : 0) |
           ((mask & FlagBits(Flag::Overflow)) ? arch::flags::Overflow : 0) |
           ((mask & FlagBits(Flag::Zero)) ? arch::flags::Zero : 0) |
           ((mask & FlagBits(Flag::Negate)) ? arch::flags::Sign : 0) |
           ((mask & FlagBits(Flag::Parity)) ? 255 : 0) |
           ((mask & FlagBits(Flag::AuxiliaryCarry)) ? arch::flags::Auxiliary : 0) |
           ((mask & FlagBits(Flag::Direction)) ? arch::flags::Direction : 0);
}

class DirectSink {
public:
    using Value = ConcreteValue;
    using Producer = Numeric;  // A local numerical snapshot, never an identity/index.

    Producer ProducerOf(Value value) const { return value.number; }

    using Operand = ConcreteOperand;
    using Result = ConcreteResult;

    DirectSink(arch::GuestState& state, const DirectServices& services)
            : state(state), services(services) {}

    Type NaturalType(Value value) const { return value.number.type; }

    OperationMetadata Metadata(const Numeric& value) const { return value.metadata; }

    Error SetMetadata(Numeric& value, OperationMetadata metadata) {
        auto flags = value.flags;
        if (metadata.profile == FlagProfile::ArithmeticRaw)
            flags = (flags & ~arch::flags::Carry) | (u64{value.raw_carry} << 29);
        MergeFlags(flags, metadata.flag_outputs);
        value.metadata = metadata;
        return Error::None;
    }

    void PublishRegisters() {}

    void PublishGPRs(std::uint16_t = UINT16_MAX) {}

    template <class Body> void IfNonZero(Value value, Body&& body) {
        if (value.number.bits != 0) body();
    }

    void FinishDispatch(u64 service) { exit_service = service; }

    void StateMemory(unsigned kind, Value address, Value requested, u64, u64 next_pc) {
        if (!services.state_image) throw detail::Rejected{BuildError::Unsupported};
        Access(services.state_image(services.opaque,
                                    kind,
                                    static_cast<u64>(address.number.bits),
                                    static_cast<u64>(requested.number.bits)));
        state.pc = next_pc;
        if ((kind & 255) == 1 || (kind & 255) == 3) exit_service = 3;
    }

    Value ReadGpr(u32 offset, unsigned, unsigned bits) {
        return LoadUniform(ScalarType(bits), {Operand::Uniform(offset, ScalarType(bits))}).value;
    }

    void WriteGpr(u32 offset, unsigned, unsigned bits, Value value) {
        StoreUniform(Type::Void,
                     {Operand::Uniform(offset, bits == 32 ? Type::U64 : ScalarType(bits)),
                      Operand::ValueOf(value, ScalarType(bits))});
    }

    Value ReadAddressGpr(unsigned) { return {}; }

    Value ReadXmm(u32 offset, unsigned) {
        return LoadUniform(Type::V128, {Operand::Uniform(offset, Type::V128)}).value;
    }

    void WriteXmm(u32 offset, unsigned, Value value) {
        StoreUniform(Type::Void,
                     {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value, Type::V128)});
    }

    void InvalidateXmm(unsigned) {}

    Value X87Zero() { return Number(0, Type::U64); }

    Value X87Address(u32 offset) {
        return Number(reinterpret_cast<std::uintptr_t>(&state) + offset, Type::U64);
    }

    std::pair<arch::X87Command, unsigned> X87Window(arch::X87Command command,
                                                    unsigned,
                                                    const InstructionData&,
                                                    InstructionRange,
                                                    std::span<const std::uint8_t>) const {
        return {command, 1};
    }

    static Type ScalarType(unsigned bits) {
        return bits == 8 ? Type::U8 : bits == 16 ? Type::U16 : bits == 32 ? Type::U32 : Type::U64;
    }

    Value AddressExpression(Type type, std::initializer_list<Operand> operands) {
        return Apply<OpCode::GetOperand>(type, operands).value;
    }

    Value ShiftedAddress(TargetInfo, Type, Value, Value, std::uint8_t) { return {}; }

    void PrepareFault(StateLayout, u64 pc, TargetInfo, bool) { state.pc = pc; }

    template <OpCode op>
    Result Apply(Type type, std::initializer_list<Operand> input, OperationMetadata metadata = {}) {
        auto value = Compute<op>(type, {input.begin(), input.size()});
        if (metadata.flag_outputs) SetMetadata(value.number, metadata);
        return {Error::None, value};
    }

#define INST(name, ...)                                                                            \
    Result name(                                                                                   \
            Type type, std::initializer_list<Operand> operands, OperationMetadata metadata = {}) { \
        return Apply<OpCode::name>(type, operands, metadata);                                      \
    }
#include "runtime/ir/opcodes.inc"
#undef INST

    bool code_changed{};
    u64 exit_service{};

private:
    arch::GuestState& state;
    const DirectServices& services;

    static Type View(const Operand& operand) {
        return operand.type == Type::Void ? operand.value.type : operand.type;
    }

    static u128 Raw(const Operand& operand) {
        return operand.kind == OperandKind::Value ? operand.value.bits : operand.payload;
    }

    static u128 Read(const Operand& operand) {
        return Raw(operand) & WideMask(TypeBits(View(operand)));
    }

    static Value Number(u128 bits, Type type, bool raw = false) {
        return {{raw ? bits : bits & WideMask(TypeBits(type)), 0, 0, type, false, {}}};
    }

    static u128 Expression(std::span<const Operand> operands, std::size_t& at) {
        const auto cell = operands[at++];
        if (cell.kind != OperandKind::Expression) return Read(cell);
        const auto left = Expression(operands, at);
        if (cell.count == 1) return left;
        const auto right = Expression(operands, at);
        switch (static_cast<OperandOp>(cell.code & 255)) {
            case OperandOp::Identity:
                return left;
            case OperandOp::Add:
                return static_cast<u64>(left) + static_cast<u64>(right);
            case OperandOp::Sub:
                return static_cast<u64>(left) - static_cast<u64>(right);
            case OperandOp::AddShift:
                return static_cast<u64>(left) + (static_cast<u64>(right) << (cell.code >> 8));
            case OperandOp::Lsl:
                return static_cast<u64>(left) << (static_cast<unsigned>(right) & 63);
            case OperandOp::Lsr:
                return static_cast<u64>(left) >> (static_cast<unsigned>(right) & 63);
            case OperandOp::Asr:
                return static_cast<std::int64_t>(left) >> (static_cast<unsigned>(right) & 63);
            default:
                throw detail::Rejected{BuildError::Unsupported};
        }
    }

    static bool ConditionValue(Condition condition, u64 f) {
        const bool c = arch::flags::GetCF(f), z = arch::flags::GetZF(f), n = arch::flags::GetSF(f),
                   v = arch::flags::GetOF(f);
        switch (condition) {
            case Condition::EQ:
                return z;
            case Condition::NE:
                return !z;
            case Condition::CS:
                return c;
            case Condition::CC:
                return !c;
            case Condition::MI:
                return n;
            case Condition::PL:
                return !n;
            case Condition::VS:
                return v;
            case Condition::VC:
                return !v;
            case Condition::HI:
                return c && !z;
            case Condition::LS:
                return !c || z;
            case Condition::GE:
                return n == v;
            case Condition::LT:
                return n != v;
            case Condition::GT:
                return !z && n == v;
            case Condition::LE:
                return z || n != v;
            case Condition::AL:
            case Condition::NV:
                return true;
        }
        return false;
    }

    bool AnyFlags(u64 mask) const {
        const auto f = state.packed_flags;
        return ((mask & FlagBits(Flag::Carry)) && arch::flags::GetCF(f)) ||
               ((mask & FlagBits(Flag::Overflow)) && arch::flags::GetOF(f)) ||
               ((mask & FlagBits(Flag::Zero)) && arch::flags::GetZF(f)) ||
               ((mask & FlagBits(Flag::Negate)) && arch::flags::GetSF(f)) ||
               ((mask & FlagBits(Flag::Parity)) && arch::flags::GetPF(f)) ||
               ((mask & FlagBits(Flag::AuxiliaryCarry)) && arch::flags::GetAF(f));
    }

    void MergeFlags(u64 flags, u64 selected) {
        const auto mask = SelectedFlags(selected);
        state.packed_flags = (state.packed_flags & ~mask) | (flags & mask);
    }

    void Access(AccessResult result) {
        if (result.status == AccessStatus::Fault)
            throw DirectExit{StepKind::Fault, result.fault_address};
        if (result.status == AccessStatus::UnsupportedBeforeAccess)
            throw detail::Rejected{BuildError::Unsupported};
        code_changed |= result.code_changed;
    }

    Value MemoryRead(u64 address, Type type, bool ordered) {
        if (!services.read) throw detail::Rejected{BuildError::Unsupported};
        u128 bits{};
        Access(services.read(services.opaque,
                             address,
                             {reinterpret_cast<std::uint8_t*>(&bits), TypeBits(type) / 8},
                             ordered));
        return Number(bits, type);
    }

    void MemoryWrite(u64 address, const Operand& value, bool ordered) {
        if (!services.write) throw detail::Rejected{BuildError::Unsupported};
        const auto bits = Read(value);
        Access(services.write(
                services.opaque,
                address,
                {reinterpret_cast<const std::uint8_t*>(&bits), TypeBits(View(value)) / 8},
                ordered));
    }

    static Value Arithmetic(OpCode opcode, Type type, u64 left, u64 right, bool carry) {
        const unsigned bits = TypeBits(type);
        const auto mask = Mask(bits), sign = u64{1} << (bits - 1);
        left &= mask;
        right &= mask;
        const bool subtract =
                opcode == OpCode::Sub || opcode == OpCode::Sbb || opcode == OpCode::Neg;
        const auto extra = u64{(opcode == OpCode::Adc || opcode == OpCode::Sbb) && carry};
        const u128 wide = subtract ? u128{left} - right - extra : u128{left} + right + extra;
        const auto result = static_cast<u64>(wide) & mask;
        const bool c = subtract ? u128{left} < u128{right} + extra : (wide >> bits) != 0;
        const bool v = subtract ? ((left ^ right) & (left ^ result) & sign) != 0
                                : (~(left ^ right) & (left ^ result) & sign) != 0;
        auto out = Number(result, type);
        out.number.flags = (result & 255) | (u64{result == 0} << 30) |
                           (u64{bool(result & sign)} << 31) | (u64{c} << 29) | (u64{v} << 28) |
                           (((left ^ right ^ result) & 16) << 22);
        out.number.raw_carry = subtract ? !c : c;
        return out;
    }

    template <OpCode op> Value Compute(Type requested, std::span<const Operand> a);
    template <OpCode op> Value Vector(Type, std::span<const Operand>);
    template <OpCode op> Value Floating(Type, std::span<const Operand>);
    template <OpCode op> Value String(Type, std::span<const Operand>);
    template <OpCode op> Value Crypto(Type, std::span<const Operand>);
};

#include "interpreter_scalar.inc"
#include "interpreter_vector.inc"
#include "interpreter_float.inc"
#include "interpreter_crypto.inc"
#include "interpreter_sse42.inc"

}  // namespace

StepResult InterpretOne(arch::GuestState& state,
                        const DirectServices& services,
                        DecodeTiming* timing) {
    StepResult result;
    result.pc = result.next_pc = state.pc;
    std::array<std::uint8_t, 15> bytes{};
    if (!services.fetch) {
        result.kind = StepKind::Unsupported;
        return result;
    }
    const auto fetched = services.fetch(services.opaque, state.pc, bytes);
    if (fetched.access.status != AccessStatus::Completed && !fetched.bytes) {
        result.kind = fetched.access.status == AccessStatus::Fault ? StepKind::Fault
                                                                   : StepKind::Unsupported;
        result.fault_address = fetched.access.fault_address;
        return result;
    }
    const auto source =
            std::span<const std::uint8_t>{bytes}.first(std::min(fetched.bytes, bytes.size()));
    const auto parsed = DecodeOne(source, state.pc, timing, services.model);
    if (!parsed) {
        if (parsed.error == DecodeError::Truncated &&
            fetched.access.status != AccessStatus::Completed) {
            result.kind = fetched.access.status == AccessStatus::Fault ? StepKind::Fault
                                                                       : StepKind::Unsupported;
            result.fault_address = fetched.access.fault_address;
            return result;
        }
        result.kind = parsed.error == DecodeError::UnsupportedGuestFeature ? StepKind::Unsupported
                      : parsed.error == DecodeError::Truncated             ? StepKind::Truncated
                                                               : StepKind::InvalidEncoding;
        return result;
    }
    using Sem = detail::Semantics<DirectSink>;
    DirectSink sink{state, services};
    StateLayout layout;
    layout.gprs_offset = offsetof(arch::GuestState, gprs);
    layout.pc_offset = offsetof(arch::GuestState, pc);
    layout.fprs_offset = offsetof(arch::GuestState, xmm);
    layout.fs_base_offset = offsetof(arch::GuestState, fs_base);
    layout.gs_base_offset = offsetof(arch::GuestState, gs_base);
    layout.mxcsr_offset = offsetof(arch::GuestState, mxcsr);
    layout.x87_offset = offsetof(arch::GuestState, x87);
    layout.cpu_query = services.cpuid ? 1 : 0;
    layout.system_query = reinterpret_cast<std::uintptr_t>(&arch::QueryGuestSystem);
    layout.runtime_state_services = services.state_image != nullptr;
    layout.x87_ip_offset = offsetof(arch::GuestState, x87_ip);
    layout.x87_dp_offset = offsetof(arch::GuestState, x87_dp);
    layout.x87_opcode_offset = offsetof(arch::GuestState, x87_opcode);
    layout.ymm_high_offset = offsetof(arch::GuestState, ymm_high);
    const auto& instruction = parsed.instruction.View().Record();
    const auto next = state.pc + instruction.size;
    Sem::PendingFlags pending;
    const auto finish = [&](u64 pc) {
        pending.Flush(sink);
        state.pc = pc;
    };
    const auto branch = [&](ConcreteValue condition, u64 yes, u64 no) {
        finish(condition.number.bits ? yes : no);
    };

    struct Flow {
        decltype(finish)& finish;
        decltype(branch)& branch;
    } flow{finish, branch};

    bool ended{};
    try {
        if (!Sem::AllowedPrefixes(instruction, source))
            throw detail::Rejected{BuildError::Unsupported};
        Sem::Execute(sink,
                     layout,
                     instruction,
                     source.first(instruction.size),
                     pending,
                     result.pc,
                     next,
                     {},
                     flow,
                     ended,
                     InstructionRange{std::span<const Instruction>{&parsed.instruction, 1}},
                     source);
        pending.Flush(sink);
        if (!ended) state.pc = next;
        result.kind = sink.exit_service ? StepKind::Service : StepKind::Completed;
        result.service = sink.exit_service;
        result.instructions = 1;
        result.decoded_bytes = instruction.size;
    } catch (const detail::Rejected&) {
        result.kind = StepKind::Unsupported;
    } catch (const DirectExit& exit) {
        result.kind = exit.kind;
        result.fault_address = exit.address;
        result.service = exit.service;
    }
    result.next_pc = state.pc;
    result.code_changed = sink.code_changed;
    return result;
}

}  // namespace swift::x86
