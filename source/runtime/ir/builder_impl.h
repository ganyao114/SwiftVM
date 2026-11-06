#pragma once

// Private implementation shared by runtime Append and declaration-bound Emit.
// No second operation store, schema, input plan or admission state lives here.
#include <algorithm>
#include <type_traits>
#include <utility>

namespace swift::runtime::ir {
namespace detail {
inline constexpr auto limit = std::size_t{NodeId::invalid};

template <class T> [[gnu::noinline]] inline void Grow(std::vector<T>& values,
                                                      std::size_t needed,
                                                      std::size_t initial) {
    if (needed <= values.capacity()) return;
    const auto capacity = values.capacity();
    const auto doubled = !capacity ? initial : capacity > limit / 2 ? limit : capacity * 2;
    values.reserve(std::max(needed, doubled));
}

inline bool SignedType(Type type) noexcept { return type >= Type::S8 && type <= Type::S64; }

inline Error LiteralType(const Operand& operand, Type& type) noexcept {
    type = operand.type;
    if (!ValidType(type) || type == Type::Void) return Error::InvalidType;
    const auto bits = TypeBits(type);
    if (!bits || bits > 64) return Error::InvalidRange;
    if (bits == 64) return Error::None;
    const auto mask = (std::uint64_t{1} << bits) - 1;
    if ((operand.payload & ~mask) == 0) return Error::None;
    // A signed literal can carry its ordinary sign-extended C++ bit pattern.
    if (SignedType(type) && (operand.payload & ~mask) == ~mask &&
        (operand.payload & (std::uint64_t{1} << (bits - 1))))
        return Error::None;
    return Error::InvalidRange;
}

inline bool Optional(Signature signature) noexcept {
    return signature == Signature::OptionalData || signature == Signature::OptionalOperand ||
           signature == Signature::OptionalIntOperand;
}

template <int known_policy = -1>
inline Error ResultType(const Schema& schema, Type requested, Type first, Type& output) noexcept {
    const auto policy = known_policy < 0 ? schema.result : static_cast<ResultPolicy>(known_policy);
    if (!ValidType(requested)) return Error::InvalidType;
    switch (policy) {
        case ResultPolicy::Void:
            if (requested != Type::Void) return Error::InvalidResultType;
            output = Type::Void;
            return Error::None;
        case ResultPolicy::Call:
            output = requested;
            return Error::None;
        case ResultPolicy::Explicit:
        case ResultPolicy::IntegerExplicit:
            if (requested == Type::Void) return Error::InvalidResultType;
            output = requested;
            break;
        case ResultPolicy::Arg0:
        case ResultPolicy::IntegerArg0:
            if (first == Type::Void) return Error::InvalidResultType;
            // Arg0 supplies the default width. An explicit result keeps the
            // input view intact, including wider sources of BitExtract/And.
            output = requested == Type::Void ? first : requested;
            break;
        case ResultPolicy::Fixed:
        case ResultPolicy::IntegerFixed:
            if (requested != Type::Void && requested != schema.fixed_type)
                return Error::InvalidResultType;
            output = schema.fixed_type;
            break;
    }
    if ((policy == ResultPolicy::IntegerArg0 || policy == ResultPolicy::IntegerFixed ||
         policy == ResultPolicy::IntegerExplicit) &&
        !IntegerType(output))
        return Error::InvalidResultType;
    return Error::None;
}

inline bool WideOperand(const Operand& input) noexcept {
    return (input.kind == OperandKind::Immediate || input.kind == OperandKind::Flags ||
            input.kind == OperandKind::Lambda) &&
           input.payload > UINT32_MAX;
}
inline bool IdentityOperand(const Operand& input) noexcept {
    return input.kind == OperandKind::Expression &&
           input.code == static_cast<unsigned>(OperandOp::Identity);
}
inline std::uint64_t OperandHeader(const Operand& input, bool wide) noexcept {
    const bool counted =
            input.kind == OperandKind::Expression || input.kind == OperandKind::Arguments;
    const auto low = counted ? input.count : static_cast<std::uint32_t>(input.payload);
    const auto code = input.kind == OperandKind::Expression ? input.code : std::uint16_t{};
    return std::uint64_t{low} | (std::uint64_t{code} << 32) |
           (static_cast<std::uint64_t>(input.kind) << 48) |
           (static_cast<std::uint64_t>(input.type) << 56) | (wide ? Cell::wide_bit : 0);
}

}  // namespace detail

template <bool measure>
[[gnu::always_inline]] inline Error Builder::StoreOperand(const Operand& input,
                                                          std::size_t& word_count) {
    const bool wide = detail::WideOperand(input);
    const std::size_t words = wide ? 2 : 1;
    if constexpr (measure) {
        if (word_count > detail::limit - words) return Error::CapacityOverflow;
        word_count += words;
    } else {
        if (storage->operands.size() > detail::limit - words) return Error::CapacityOverflow;
        const auto needed = storage->operands.size() + words;
        if (needed > storage->operands.capacity()) detail::Grow(storage->operands, needed, 96);
        storage->operands.push_back({detail::OperandHeader(input, wide)});
        if (wide) storage->operands.push_back({input.payload});
    }
    return Error::None;
}

template <int known_signature, bool measure>
Error Builder::AppendArgument(Signature dynamic_signature,
                              std::span<const Operand> operands,
                              std::size_t& next,
                              Type& type,
                              std::size_t& word_count,
                              std::uint32_t before) {
    const auto signature =
            known_signature < 0 ? dynamic_signature : static_cast<Signature>(known_signature);
    if (next == operands.size())
        return detail::Optional(signature) ? Error::None : Error::MissingOperand;
    const auto atom = [&](const Operand& input, Type& type) -> Error {
        if (input.kind == OperandKind::Value) {
            if (input.payload >= NodeId::invalid) return Error::InvalidValue;
            const auto error = storage->ReadType(
                    {input.unit, NodeId{static_cast<std::uint32_t>(input.payload)}},
                    input.type,
                    type);
            if (error != Error::None) return error;
            if constexpr (measure)
                if (input.payload >= before) return Error::InvalidValue;
            return Error::None;
        }
        if (input.kind == OperandKind::Immediate) return detail::LiteralType(input, type);
        return Error::InvalidOperandKind;
    };
    const auto store = [&](const Operand& input) {
        return StoreOperand<measure>(input, word_count);
    };
    const auto& input = operands[next++];
    Error error{Error::None};
    switch (signature) {
        case Signature::Value:
        case Signature::IntValue:
        case Signature::Int32Value:
        case Signature::Int64Value:
            if (input.kind != OperandKind::Value)
                error = Error::InvalidOperandKind;
            else
                error = atom(input, type);
            if (error == Error::None && signature != Signature::Value &&
                (!IntegerType(type) ||
                 (signature == Signature::Int32Value && TypeBits(type) != 32) ||
                 (signature == Signature::Int64Value && TypeBits(type) != 64)))
                error = Error::InvalidType;
            break;
        case Signature::Imm:
        case Signature::IntImm:
            if (input.kind != OperandKind::Immediate)
                error = Error::InvalidOperandKind;
            else
                error = detail::LiteralType(input, type);
            if (error == Error::None && signature == Signature::IntImm && !IntegerType(type))
                error = Error::InvalidType;
            break;
        case Signature::Uniform:
        case Signature::Local:
            if (input.kind !=
                (signature == Signature::Uniform ? OperandKind::Uniform : OperandKind::Local))
                error = Error::InvalidOperandKind;
            else if (!ValidType(input.type) || input.type == Type::Void)
                error = Error::InvalidType;
            else if (input.payload > UINT32_MAX)
                error = Error::InvalidRange;
            else
                type = input.type;
            break;
        case Signature::Cond:
            if (input.kind != OperandKind::Condition)
                error = Error::InvalidOperandKind;
            else if (input.payload >= 16)
                error = Error::InvalidRange;
            break;
        case Signature::Flags:
            if (input.kind != OperandKind::Flags) error = Error::InvalidOperandKind;
            break;
        case Signature::OptionalData:
            error = atom(input, type);
            break;
        case Signature::Operand:
        case Signature::IntOperand:
        case Signature::OptionalOperand:
        case Signature::OptionalIntOperand: {
            const bool integer = signature == Signature::IntOperand ||
                                 signature == Signature::OptionalIntOperand;
            // A bare atom is the ordinary unary operand. Legacy Identity
            // still checks its real header and child before omitting it.
            if (input.kind == OperandKind::Value || input.kind == OperandKind::Immediate) {
                error = atom(input, type);
                if (error == Error::None && integer && !IntegerType(type))
                    error = Error::InvalidType;
                break;
            }
            if (input.kind != OperandKind::Expression) return Error::InvalidOperandKind;
            const auto expression = static_cast<unsigned>(input.code) & 0xffu;
            const auto shift = static_cast<unsigned>(input.code) >> 8;
            if (expression > static_cast<unsigned>(OperandOp::AddShift) || shift >= 64 ||
                (shift && expression != static_cast<unsigned>(OperandOp::AddShift) &&
                 expression != static_cast<unsigned>(OperandOp::AddExtend)) ||
                input.count != (expression == static_cast<unsigned>(OperandOp::Identity) ? 1u : 2u))
                return Error::InvalidRange;
            if (input.count > operands.size() - next) return Error::MissingOperand;
            if (!detail::IdentityOperand(input)) {
                error = store(input);
                if (error != Error::None) return error;
            }
            for (std::uint32_t part = 0; part < input.count; ++part) {
                const auto& child = operands[next++];
                Type item;
                error = atom(child, item);
                if (error == Error::None && integer && !IntegerType(item))
                    error = Error::InvalidType;
                if (error != Error::None) return error;
                if (!part) type = item;
                error = store(child);
                if (error != Error::None) return error;
            }
            return Error::None;
        }
        case Signature::Lambda: {
            if (input.kind != OperandKind::Lambda) return Error::InvalidOperandKind;
            if (next == operands.size()) return Error::MissingOperand;
            error = store(input);
            if (error != Error::None) return error;
            const auto& child = operands[next++];
            error = atom(child, type);
            if (error == Error::None && !IntegerType(type)) error = Error::InvalidType;
            return error == Error::None ? store(child) : error;
        }
        case Signature::Params:
            if (input.kind != OperandKind::Arguments) return Error::InvalidOperandKind;
            if (input.count > operands.size() - next) return Error::MissingOperand;
            error = store(input);
            if (error != Error::None) return error;
            for (std::uint32_t part = 0; part < input.count; ++part) {
                const auto& child = operands[next++];
                Type item;
                error = atom(child, item);
                if (error != Error::None) return error;
                error = store(child);
                if (error != Error::None) return error;
            }
            return Error::None;
    }
    return error == Error::None ? store(input) : error;
}

template <OpCode known_op, bool measure>
Error Builder::ProcessArguments(const Schema& schema,
                                std::span<const Operand> operands,
                                Type& first,
                                std::size_t& word_count,
                                std::uint32_t before) {
    std::size_t next{};
    Error error{Error::None};
    if constexpr (known_op == OpCode::Count) {
        for (std::size_t argument = 0; argument < schema.argument_count; ++argument) {
            Type type{Type::Void};
            error = AppendArgument<-1, measure>(
                    schema.arguments[argument], operands, next, type, word_count, before);
            if (error != Error::None) return error;
            if (!argument) first = type;
        }
    } else {
        // This expands only the declaration's actual logical arguments.
        // The signature and result policy are constants; cell tags, values,
        // consumer views and handle identity remain real runtime inputs.
        [&]<std::size_t... argument>(std::index_sequence<argument...>) {
            const auto check = [&]<std::size_t at>() {
                if (error != Error::None) return;
                Type type{Type::Void};
                constexpr auto signature =
                        schemas[static_cast<std::size_t>(known_op)].arguments[at];
                error = AppendArgument<static_cast<int>(signature), measure>(
                        signature, operands, next, type, word_count, before);
                if constexpr (at == 0) first = type;
            };
            (check.template operator()<argument>(), ...);
        }(std::make_index_sequence<schemas[static_cast<std::size_t>(known_op)].argument_count>{});
        if (error != Error::None) return error;
    }
    if (next != operands.size()) return Error::ExtraOperand;
    return Error::None;
}

template <OpCode known_op> AppendResult Builder::AppendImpl(OpCode op,
                                                            Type requested,
                                                            std::span<const Operand> operands,
                                                            OperationMetadata metadata) {
    if (!storage) return {Error::Consumed};
    if (!storage->active) return {Error::NoActiveBlock};
    const auto* schema = [&] {
        if constexpr (known_op == OpCode::Count)
            return SchemaFor(op);
        else
            return &schemas[static_cast<std::size_t>(known_op)];
    }();
    if (!schema) return {Error::InvalidOpcode};
    if (static_cast<unsigned>(metadata.profile) >
                static_cast<unsigned>(FlagProfile::LogicalZeroCVA) ||
        (metadata.discard_omitted_flags && !metadata.flag_outputs))
        return {Error::InvalidRange};
    const auto node_index = storage->operations.size();
    if (node_index == detail::limit) return {Error::CapacityOverflow};

    // This private append owns its whole mutation interval. Each actual cell
    // is installed as it is checked; failure removes only this unpublished
    // suffix. Cell destruction is trivial and shrinking cannot allocate.
    struct OperandRollback {
        std::vector<Cell>& cells;
        const std::size_t first;
        bool committed{};
        ~OperandRollback() noexcept {
            if (!committed) cells.resize(first);
        }
    } rollback{storage->operands, storage->operands.size()};

    Type first{Type::Void};
    std::size_t word_count{};  // Used only by the rewrite measurement instantiation.
    if (const auto error = ProcessArguments<known_op, false>(
                *schema, operands, first, word_count, NodeId::invalid);
        error != Error::None)
        return {error};
    Type type;
    Error result_error;
    if constexpr (known_op == OpCode::Count)
        result_error = detail::ResultType(*schema, requested, first, type);
    else
        result_error = detail::ResultType<static_cast<int>(
                schemas[static_cast<std::size_t>(known_op)].result)>(
                *schema, requested, first, type);
    if (result_error != Error::None) return {result_error};
    // All cell admission has finished. The last possible allocation precedes
    // publishing the operation, its ID and the block's actual node count.
    if (node_index == storage->operations.capacity())
        detail::Grow(storage->operations, node_index + 1, 32);
    static_assert(std::is_trivially_copyable_v<Operation> && std::is_trivially_copyable_v<Cell>);
    const NodeId node{static_cast<std::uint32_t>(node_index)};
    const auto first_operand = static_cast<std::uint32_t>(rollback.first);
    storage->operations.push_back(
            {metadata.flag_outputs,
             first_operand,
             static_cast<std::uint32_t>(storage->operands.size() - rollback.first),
             storage->active,
             op,
             type,
             static_cast<std::uint8_t>(static_cast<unsigned>(metadata.profile) |
                                       (metadata.branch_only ? 4u : 0u) |
                                       (metadata.discard_omitted_flags ? 8u : 0u))});
    ++storage->blocks[storage->active.value].node_count;
    rollback.committed = true;
    return {Error::None, node, type == Type::Void ? 0 : storage->unit};
}

}  // namespace swift::runtime::ir
