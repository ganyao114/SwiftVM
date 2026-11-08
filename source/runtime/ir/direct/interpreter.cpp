#include "interpreter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

#include "runtime/arch/x86/bit_operations.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/arch/x86/floating.h"
#include "runtime/arch/x86/sse42_string.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/arch/x86/x87.h"
#include "runtime/platform/fp_environment.h"

namespace swift::runtime::ir::direct {
namespace {
namespace arch = ::swift::runtime::x86;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u128 = unsigned __int128;
using s128 = __int128;

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

}  // namespace

Error Interpreter::SetMetadata(Producer& value, OperationMetadata metadata) {
    auto flags = value.flags;
    if (metadata.profile == FlagProfile::ArithmeticRaw)
        flags = (flags & ~arch::flags::Carry) | (u64{value.raw_carry} << 29);
    MergeFlags(flags, metadata.flag_outputs);
    value.metadata = metadata;
    return Error::None;
}

Type Interpreter::View(const Operand& operand) {
    return operand.type == Type::Void ? operand.value.type : operand.type;
}

u128 Interpreter::Raw(const Operand& operand) {
    return operand.kind == OperandKind::Value ? operand.value.bits : operand.payload;
}

u128 Interpreter::Read(const Operand& operand) {
    return Raw(operand) & WideMask(TypeBits(View(operand)));
}

Interpreter::Value Interpreter::Number(u128 bits, Type type, bool raw) {
    return {{raw ? bits : bits & WideMask(TypeBits(type)), 0, 0, type, false, {}}};
}

u128 Interpreter::Expression(std::span<const Operand> operands, std::size_t& at) {
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
            throw Exit{ExitKind::Unsupported};
    }
}

bool Interpreter::ConditionValue(Condition condition, u64 f) {
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

bool Interpreter::AnyFlags(u64 mask) const {
    const auto f = environment.packed_flags;
    return ((mask & FlagBits(Flag::Carry)) && arch::flags::GetCF(f)) ||
           ((mask & FlagBits(Flag::Overflow)) && arch::flags::GetOF(f)) ||
           ((mask & FlagBits(Flag::Zero)) && arch::flags::GetZF(f)) ||
           ((mask & FlagBits(Flag::Negate)) && arch::flags::GetSF(f)) ||
           ((mask & FlagBits(Flag::Parity)) && arch::flags::GetPF(f)) ||
           ((mask & FlagBits(Flag::AuxiliaryCarry)) && arch::flags::GetAF(f));
}

void Interpreter::MergeFlags(u64 flags, u64 selected) {
    const auto mask = SelectedFlags(selected);
    environment.packed_flags = (environment.packed_flags & ~mask) | (flags & mask);
    if (mask) observed_effects = true;
}

void Interpreter::ObserveAccess(AccessResult result) {
    if (result.status == AccessStatus::Fault) throw Exit{ExitKind::Fault, result.fault_address};
    if (result.status == AccessStatus::UnsupportedBeforeAccess) throw Exit{ExitKind::Unsupported};
    observed_effects = true;
    code_changed |= result.code_changed;
}

Interpreter::Value Interpreter::MemoryRead(u64 address, Type type, bool ordered) {
    if (!services.read) throw Exit{ExitKind::Unsupported};
    u128 bits{};
    ObserveAccess(services.read(services.opaque,
                                address,
                                {reinterpret_cast<std::uint8_t*>(&bits), TypeBits(type) / 8},
                                ordered));
    return Number(bits, type);
}

void Interpreter::MemoryWrite(u64 address, const Operand& value, bool ordered) {
    if (!services.write) throw Exit{ExitKind::Unsupported};
    const auto bits = Read(value);
    ObserveAccess(services.write(
            services.opaque,
            address,
            {reinterpret_cast<const std::uint8_t*>(&bits), TypeBits(View(value)) / 8},
            ordered));
}

Interpreter::Value Interpreter::Arithmetic(
        OpCode opcode, Type type, u64 left, u64 right, bool carry) {
    const unsigned bits = TypeBits(type);
    const auto mask = Mask(bits), sign = u64{1} << (bits - 1);
    left &= mask;
    right &= mask;
    const bool subtract = opcode == OpCode::Sub || opcode == OpCode::Sbb || opcode == OpCode::Neg;
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

template <OpCode op> Interpreter::Result Interpreter::Emit(Type type,
                                                           std::span<const Operand> operands,
                                                           OperationMetadata metadata) {
    auto value = Handle<op>(type, operands);
    if (metadata.flag_outputs) SetMetadata(value.number, metadata);
    return {Error::None, value};
}

#include "crypto.inc"
#include "float.inc"
#include "scalar.inc"
#include "sse42.inc"
#include "vector.inc"

// The declaration table is also the complete static call surface. This does
// not introduce a runtime opcode registry or an intermediate instruction list.
#define INST(name, ...)                                                                            \
    template Interpreter::Result Interpreter::Emit<OpCode::name>(                                  \
            Type, std::span<const Operand>, OperationMetadata);
#include "runtime/ir/opcodes.inc"
#undef INST

}  // namespace swift::runtime::ir::direct
