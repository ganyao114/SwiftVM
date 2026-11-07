#include "builder.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace swift::runtime::ir {
namespace {
constexpr auto limit = std::size_t{NodeId::invalid};

bool Empty(Value value) noexcept { return !value.unit && !value.node; }

bool Empty(BlockRef block) noexcept { return !block.unit && !block.id; }
}  // namespace

Builder::Builder(Scope scope, TargetInfo target) : Builder(scope, target, Program{}) {}

Builder::Builder(Scope scope, TargetInfo target, Program reusable)
        : storage(std::move(reusable.storage)) {
    if ((scope != Scope::Block && scope != Scope::Function) ||
        static_cast<unsigned>(target.mode) > static_cast<unsigned>(TargetMode::Unknown))
        throw std::invalid_argument("invalid IR request metadata");
    if (storage)
        storage->Reset(scope, target);
    else
        storage = std::make_unique<Program::Storage>(scope, target);
}

Builder::~Builder() = default;
Builder::Builder(Builder&&) noexcept = default;
Builder& Builder::operator=(Builder&&) noexcept = default;

Builder::Builder(std::unique_ptr<Program::Storage> input, std::size_t max_words) noexcept
        : storage(std::move(input)), edit_only(true), max_operand_words(max_words) {}

Builder Builder::Edit(Program&& program, std::size_t max_words) noexcept {
    return Builder{std::move(program.storage), max_words};
}

Error Builder::Reserve(std::size_t nodes, std::size_t operand_words, std::size_t blocks) {
    if (!storage) return Error::Consumed;
    if (nodes > limit || operand_words > limit || blocks > limit) return Error::CapacityOverflow;
    storage->operations.reserve(nodes);
    storage->operands.reserve(operand_words);
    storage->blocks.reserve(blocks);
    return Error::None;
}

BlockResult Builder::CreateBlock() {
    if (!storage) return {Error::Consumed};
    if (edit_only) return {Error::EditOnly};
    if (storage->scope == Scope::Block && !storage->blocks.empty()) return {Error::ScopeMismatch};
    if (storage->blocks.size() == limit) return {Error::CapacityOverflow};
    if (storage->blocks.size() == storage->blocks.capacity())
        detail::Grow(storage->blocks,
                     storage->blocks.size() + 1,
                     storage->scope == Scope::Block ? 1 : 4);
    const BlockRef block{storage->unit,
                         BlockId{static_cast<std::uint32_t>(storage->blocks.size())}};
    storage->blocks.push_back({});
    if (storage->blocks.size() == 1) storage->entry = block.id;
    ++storage->unfinished;
    return {Error::None, block};
}

Error Builder::SetEntry(BlockRef block) noexcept {
    if (!storage) return Error::Consumed;
    if (const auto error = storage->CheckBlock(block); error != Error::None) return error;
    storage->entry = block.id;
    return Error::None;
}

Error Builder::BeginBlock(BlockRef block) {
    if (!storage) return Error::Consumed;
    if (const auto error = storage->CheckBlock(block); error != Error::None) return error;
    if (storage->active) return Error::BlockAlreadyOpen;
    auto& row = storage->blocks[block.id.value];
    if (row.State() == BlockState::Closed) return Error::BlockAlreadyEnded;
    row.first_node = static_cast<std::uint32_t>(storage->operations.size());
    row.terminal.SetState(BlockState::Open);
    storage->active = block.id;
    return Error::None;
}

AppendResult Builder::Append(OpCode op,
                             Type requested,
                             std::span<const Operand> operands,
                             OperationMetadata metadata) {
    return AppendImpl<OpCode::Count>(op, requested, operands, metadata);
}

Error Builder::SetMetadata(NodeId local, OperationMetadata metadata) noexcept {
    if (!storage) return Error::Consumed;
    if (!local || local.value >= storage->operations.size()) return Error::InvalidValue;
    if (static_cast<unsigned>(metadata.profile) >
                static_cast<unsigned>(FlagProfile::LogicalZeroCVA) ||
        (metadata.discard_omitted_flags && !metadata.flag_outputs))
        return Error::InvalidRange;
    auto& record = storage->operations[local.value];
    record.flag_outputs = metadata.flag_outputs;
    record.properties = static_cast<std::uint8_t>(static_cast<unsigned>(metadata.profile) |
                                                  (metadata.branch_only ? 4u : 0u) |
                                                  (metadata.discard_omitted_flags ? 8u : 0u));
    return Error::None;
}

Error Builder::ReplaceValueUse(NodeId consumer,
                               CellRange::Iterator edge,
                               Value replacement,
                               Type view) noexcept {
    if (!storage) return Error::Consumed;
    if (!consumer || consumer.value >= storage->operations.size()) return Error::InvalidValue;
    const auto& record = storage->operations[consumer.value];
    const auto size = storage->operands.size();
    if (!record.operand_count || record.first_operand > size ||
        record.operand_count > size - record.first_operand)
        return Error::InvalidRange;
    auto* begin = storage->operands.data() + record.first_operand;
    const auto* end = begin + record.operand_count;
    // Iterators can only originate at a real range boundary and advance over
    // whole logical cells. Matching its end binds this edge to the actual
    // consumer range, without rescanning earlier cells or an index directory.
    if (edge.end != end || edge.current < begin || edge.current >= end) return Error::InvalidRange;
    const auto cell = *edge;
    if (cell.Kind() != OperandKind::Value || (edge.current->bits & Cell::wide_bit))
        return Error::InvalidOperandKind;
    Type previous, next;
    if (const auto error = storage->ReadType(
                {storage->unit, NodeId{static_cast<std::uint32_t>(cell.Payload())}},
                cell.TypeView(),
                previous);
        error != Error::None)
        return error;
    if (const auto error = storage->ReadType(replacement, view, next); error != Error::None)
        return error;
    if (replacement.node.value >= consumer.value) return Error::InvalidValue;
    if (next != previous) return Error::InvalidType;
    auto& word = storage->operands[record.first_operand + (edge.current - begin)];
    constexpr auto changed = std::uint64_t{UINT32_MAX} | (std::uint64_t{UINT8_MAX} << 56);
    word.bits = (word.bits & ~changed) | replacement.node.value |
                (static_cast<std::uint64_t>(view) << 56);
    return Error::None;
}

Error Builder::Rewrite(NodeId local,
                       OpCode op,
                       Type requested,
                       std::span<const Operand> operands,
                       OperationMetadata metadata) {
    if (!storage) return Error::Consumed;
    if (!local || local.value >= storage->operations.size()) return Error::InvalidValue;
    const auto* schema = SchemaFor(op);
    if (!schema) return Error::InvalidOpcode;
    if (static_cast<unsigned>(metadata.profile) >
                static_cast<unsigned>(FlagProfile::LogicalZeroCVA) ||
        (metadata.discard_omitted_flags && !metadata.flag_outputs))
        return Error::InvalidRange;

    // Same local schema and source checks as append, without writing or
    // allocating. Only this operation's actual inputs are visited.
    Type first{Type::Void}, type;
    std::size_t words{};
    if (const auto error =
                ProcessArguments<OpCode::Count, true>(*schema, operands, first, words, local.value);
        error != Error::None)
        return error;
    if (const auto error = detail::ResultType(*schema, requested, first, type);
        error != Error::None)
        return error;
    auto& record = storage->operations[local.value];
    if (type != record.type) return Error::InvalidResultType;
    const auto old_size = storage->operands.size();
    if (record.operand_count &&
        (record.first_operand > old_size || record.operand_count > old_size - record.first_operand))
        return Error::InvalidRange;
    const bool tail = record.operand_count &&
                      std::size_t{record.first_operand} + record.operand_count == old_size;
    const auto start = record.operand_count && (words <= record.operand_count || tail)
                               ? record.first_operand
                               : old_size;
    if (start > detail::limit - words) return Error::CapacityOverflow;
    const auto end = start + words;
    const auto size = tail || start == old_size ? end : old_size;
    if (size > max_operand_words) return Error::EditBudgetExceeded;

    // All allocation precedes any changed payload/header. The immutable
    // input span and sole mutable owner remain in this synchronous call.
    // Encode directly into the one arena; no decoded draft is produced.
    if (size > storage->operands.capacity()) detail::Grow(storage->operands, size, 96);
    storage->operands.resize(size);
    auto at = start;
    for (const auto& input : operands) {
        if (detail::IdentityOperand(input)) continue;
        const bool wide = detail::WideOperand(input);
        storage->operands[at++] = {detail::OperandHeader(input, wide)};
        if (wide) storage->operands[at++] = {input.payload};
    }
    record = {metadata.flag_outputs,
              static_cast<std::uint32_t>(start),
              static_cast<std::uint32_t>(words),
              record.block,
              op,
              type,
              static_cast<std::uint8_t>(static_cast<unsigned>(metadata.profile) |
                                        (metadata.branch_only ? 4u : 0u) |
                                        (metadata.discard_omitted_flags ? 8u : 0u))};
    return Error::None;
}

Error Builder::EraseUnused(NodeId local) noexcept {
    if (!storage) return Error::Consumed;
    if (!edit_only) return Error::EditOnly;
    if (!local || local.value >= storage->operations.size()) return Error::InvalidValue;
    auto& record = storage->operations[local.value];
    constexpr auto reads =
            Effect::ReadGuestState | Effect::ReadLocal | Effect::ReadHostState | Effect::ReadFlags;
    const auto effects = static_cast<unsigned>(EffectsFor(record.opcode, record.Metadata()));
    if (record.flag_outputs || record.properties || (effects & ~static_cast<unsigned>(reads)))
        return Error::ObservableOperation;
    record = {0, 0, 0, record.block, OpCode::Nop, Type::Void, 0};
    return Error::None;
}

Error Builder::EndBlock(Terminal terminal) {
    if (!storage) return Error::Consumed;
    if (!storage->active) return Error::NoActiveBlock;
    if (terminal.kind == TerminalKind::Incomplete) return Error::IncompleteTerminal;
    Type type;
    const auto no_value = [&] {
        return !terminal.value_present && Empty(terminal.value) &&
               terminal.value_type == Type::Void;
    };
    switch (terminal.kind) {
        case TerminalKind::Return:
            if (!Empty(terminal.first) || !Empty(terminal.second)) return Error::InvalidRange;
            if (terminal.value_present) {
                if (const auto error = storage->ReadType(terminal.value, terminal.value_type, type);
                    error != Error::None)
                    return error;
            } else if (!no_value())
                return Error::InvalidValue;
            break;
        case TerminalKind::Jump:
            if (!no_value() || !Empty(terminal.second)) return Error::InvalidRange;
            if (const auto error = storage->CheckBlock(terminal.first); error != Error::None)
                return error;
            break;
        case TerminalKind::Branch:
            if (!terminal.value_present) return Error::InvalidValue;
            if (const auto error = storage->ReadType(terminal.value, terminal.value_type, type);
                error != Error::None)
                return error;
            if (!IntegerType(type)) return Error::InvalidType;
            if (const auto error = storage->CheckBlock(terminal.first); error != Error::None)
                return error;
            if (const auto error = storage->CheckBlock(terminal.second); error != Error::None)
                return error;
            break;
        case TerminalKind::Unreachable:
        case TerminalKind::Dispatch:
            if (!no_value() || !Empty(terminal.first) || !Empty(terminal.second))
                return Error::InvalidRange;
            break;
        default:
            return Error::InvalidRange;
    }
    auto& block = storage->blocks[storage->active.value];
    block.terminal = {terminal.value.node,
                      terminal.first.id,
                      terminal.second.id,
                      terminal.kind,
                      terminal.value_type,
                      static_cast<std::uint8_t>((static_cast<unsigned>(BlockState::Closed) << 1) |
                                                (terminal.value_present ? 1u : 0u)),
                      0};
    storage->active = {};
    --storage->unfinished;
    return Error::None;
}

WorkCounts Builder::CountWork() const noexcept {
    return storage ? storage->CountWork() : WorkCounts{};
}

SubmitResult Builder::Submit() noexcept {
    if (!storage) return {Error::Consumed, {}};
    if (storage->blocks.empty() || storage->active || storage->unfinished || !storage->entry ||
        storage->entry.value >= storage->blocks.size())
        return {Error::IncompleteScope, {}};
    return {Error::None, Program{std::move(storage)}};
}

}  // namespace swift::runtime::ir
