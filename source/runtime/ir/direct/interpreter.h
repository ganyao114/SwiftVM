#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>

#include "runtime/ir/opcode.h"
#include "services.h"

namespace swift::runtime::ir::direct {

struct Environment {
    std::span<std::uint8_t> uniforms;
    std::uint64_t& packed_flags;
    std::uint64_t& pc;
    std::uint32_t& mxcsr;
};

enum class ExitKind : std::uint8_t { Unsupported, Fault, Service };

struct Exit {
    ExitKind kind;
    std::uint64_t address{}, service{};
};

// A numerical snapshot, without a Program identity, node or persistent table.
struct Producer {
    unsigned __int128 bits{};
    std::uint64_t flags{}, extra{};
    Type type{Type::Void};
    bool raw_carry{};
    OperationMetadata metadata;

    explicit operator bool() const noexcept { return type != Type::Void; }
};

struct Value {
    Producer number;

    explicit operator bool() const noexcept { return bool(number); }
};

struct Operand {
    OperandKind kind{};
    Type type{Type::Void};
    Producer value;
    std::uint64_t payload{};
    unsigned count{}, code{};

    static Operand ValueOf(Value value, Type view = Type::Void) {
        return {OperandKind::Value, view, value.number};
    }

    static Operand Immediate(std::uint64_t value, Type type = Type::U64) {
        return {OperandKind::Immediate, type, {}, value};
    }

    static Operand Uniform(std::uint32_t offset, Type type) {
        return {OperandKind::Uniform, type, {}, offset};
    }

    static Operand Flags(Flag value) {
        return {OperandKind::Flags, Type::Void, {}, FlagBits(value)};
    }

    static Operand Flags(std::uint64_t value) {
        return {OperandKind::Flags, Type::Void, {}, value};
    }

    static Operand Condition(ir::Condition value) {
        return {OperandKind::Condition, Type::Void, {}, static_cast<std::uint64_t>(value)};
    }

    static Operand Expression(OperandOp op, unsigned count, unsigned shift = 0) {
        return {OperandKind::Expression,
                Type::Void,
                {},
                0,
                count,
                static_cast<unsigned>(op) | (shift << 8)};
    }
};

struct Result {
    Error error{Error::None};
    Value value;

    explicit operator bool() const noexcept { return error == Error::None; }

    Value GetValue() const { return value; }
};

// Executes one statically selected neutral IR operation. It owns no decoder,
// Program, architectural register layout or instruction traversal.
class Interpreter {
public:
    using Value = direct::Value;
    using Operand = direct::Operand;
    using Result = direct::Result;
    using Producer = direct::Producer;

    explicit Interpreter(Environment environment, Services services = {})
            : environment(environment), services(services) {}

    Producer ProducerOf(Value value) const { return value.number; }

    Type NaturalType(Value value) const { return value.number.type; }

    OperationMetadata Metadata(const Producer& value) const { return value.metadata; }

    Error SetMetadata(Producer&, OperationMetadata);

    template <OpCode op> Result Emit(Type, std::span<const Operand>, OperationMetadata = {});

    template <OpCode op> Result Emit(Type type,
                                     std::initializer_list<Operand> operands,
                                     OperationMetadata metadata = {}) {
        return Emit<op>(type, {operands.begin(), operands.size()}, metadata);
    }

#define INST(name, ...)                                                                            \
    Result name(                                                                                   \
            Type type, std::initializer_list<Operand> operands, OperationMetadata metadata = {}) { \
        return Emit<OpCode::name>(type, {operands.begin(), operands.size()}, metadata);            \
    }
#include "runtime/ir/opcodes.inc"
#undef INST

    void ObserveAccess(AccessResult);

    bool code_changed{};
    // A later unsupported access cannot make an already observed fragment
    // safe to replay through a different consumer.
    bool observed_effects{};

private:
    using u64 = std::uint64_t;
    using u128 = unsigned __int128;

    Environment environment;
    Services services;

    static Type View(const Operand&);
    static u128 Raw(const Operand&);
    static u128 Read(const Operand&);
    static Value Number(u128 bits, Type type, bool raw = false);
    static u128 Expression(std::span<const Operand>, std::size_t&);
    static bool ConditionValue(ir::Condition, u64 flags);
    bool AnyFlags(u64 mask) const;
    void MergeFlags(u64 flags, u64 selected);
    Value MemoryRead(u64 address, Type, bool ordered);
    void MemoryWrite(u64 address, const Operand&, bool ordered);
    static Value Arithmetic(OpCode, Type, u64 left, u64 right, bool carry);

    template <OpCode op> Value Handle(Type, std::span<const Operand>);
    template <OpCode op> Value HandleVector(Type, std::span<const Operand>);
    template <OpCode op> Value HandleFloating(Type, std::span<const Operand>);
    template <OpCode op> Value HandleString(Type, std::span<const Operand>);
    template <OpCode op> Value HandleCrypto(Type, std::span<const Operand>);
};

}  // namespace swift::runtime::ir::direct
