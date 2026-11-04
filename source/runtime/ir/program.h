#pragma once

#include <memory>
#include <span>
#include <vector>
#include "opcode.h"

namespace swift::runtime::ir {

// Explicit observation of actual storage, including an unfinished request.
// Value uses include terminal inputs; words include retired physical ranges.
struct WorkCounts {
    std::size_t nodes{}, value_uses{}, operand_words{}, blocks{};
    friend bool operator==(WorkCounts, WorkCounts) = default;
};

struct Operation {
    std::uint64_t flag_outputs{};
    // With zero words there is no payload; first_operand is immaterial.
    std::uint32_t first_operand{}, operand_count{};
    BlockId block{};
    OpCode opcode{};
    Type type{Type::Void};
    std::uint8_t properties{};
    OperationMetadata Metadata() const noexcept {
        return {flag_outputs, static_cast<FlagProfile>(properties & 3), bool(properties & 4), bool(properties & 8)};
    }
};
static_assert(sizeof(Operation) == 24);
enum class BlockState : std::uint8_t { Declared, Open, Closed };
struct StoredTerminal {
    NodeId value{};
    BlockId first{}, second{};
    TerminalKind kind{TerminalKind::Incomplete};
    Type value_type{Type::Void};
    std::uint8_t properties{}, reserved{};
    bool HasValue() const noexcept { return properties & 1; }
    BlockState State() const noexcept { return static_cast<BlockState>((properties >> 1) & 3); }

private:
    void SetState(BlockState state) noexcept {
        properties = static_cast<std::uint8_t>((properties & ~6u) | (static_cast<unsigned>(state) << 1));
    }
    friend class Builder;
};
static_assert(sizeof(StoredTerminal) == 16);
struct BlockRecord {
    std::uint32_t first_node{}, node_count{};
    StoredTerminal terminal{};
    BlockState State() const noexcept { return terminal.State(); }
};
static_assert(sizeof(BlockRecord) == 24);

class Builder;
class Program {
public:
    Program() noexcept;
    ~Program();
    Program(Program&&) noexcept;
    Program& operator=(Program&&) noexcept;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    explicit operator bool() const noexcept { return bool(storage); }
    std::uint64_t UnitId() const noexcept { return storage ? storage->unit : 0; }
    Scope ScopeKind() const noexcept { return storage ? storage->scope : Scope::Block; }
    TargetInfo Target() const noexcept { return storage ? storage->target : TargetInfo{}; }
    // The actual entry in this owner, independent of declaration/body order.
    // Empty or consumed owners return the invalid BlockId.
    BlockId EntryBlock() const noexcept { return storage ? storage->entry : BlockId{}; }
    std::span<const Operation> Operations() const noexcept {
        return storage ? std::span<const Operation>{storage->operations} : std::span<const Operation>{};
    }
    // Physical arena only: rewrites can leave retired/unclaimed words. Live
    // operands are the operation-local ranges below, never this raw span.
    std::span<const Cell> OperandArena() const noexcept {
        return storage ? std::span<const Cell>{storage->operands} : std::span<const Cell>{};
    }
    std::span<const BlockRecord> Blocks() const noexcept {
        return storage ? std::span<const BlockRecord>{storage->blocks} : std::span<const BlockRecord>{};
    }
    CellRange Operands(NodeId local) const noexcept {
        return storage ? storage->Operands(local) : CellRange{};
    }
    // Only the actual record borrowed from this Program is accepted. Copies
    // and foreign records are rejected; use the explicit local-ID overload.
    CellRange Operands(const Operation&) const noexcept;
    bool Contains(Value) const noexcept;
    // Vector payload/capacity bytes, excluding allocator bookkeeping.
    std::size_t StorageBytes() const noexcept;
    std::size_t ReservedBytes() const noexcept;

private:
    struct Storage {
        Storage(Scope, TargetInfo);
        Error ReadType(Value value, Type view, Type& result) const noexcept {
            if (!value.unit || !value.node) return Error::InvalidValue;
            if (value.unit != unit) return Error::ForeignUnit;
            if (value.node.value >= operations.size() ||
                operations[value.node.value].type == Type::Void)
                return Error::InvalidValue;
            if (!ValidType(view)) return Error::InvalidType;
            result = view == Type::Void ? operations[value.node.value].type : view;
            return Error::None;
        }
        Error CheckBlock(BlockRef) const noexcept;
        CellRange Operands(NodeId local) const noexcept {
            if (!local || local.value >= operations.size()) return {};
            const auto& op = operations[local.value];
            if (!op.operand_count) return {}; // Empty ranges own no arena position.
            const std::span<const Cell> words{operands};
            if (op.first_operand > words.size() || op.operand_count > words.size() - op.first_operand)
                return {};
            return CellRange{words.subspan(op.first_operand, op.operand_count)};
        }
        WorkCounts CountWork() const noexcept;
        const std::uint64_t unit;
        const Scope scope;
        BlockId entry{};
        const TargetInfo target;
        std::vector<Operation> operations;
        std::vector<Cell> operands;
        std::vector<BlockRecord> blocks;
        BlockId active{};
        std::uint32_t unfinished{};
    };
    std::unique_ptr<Storage> storage;
    explicit Program(std::unique_ptr<Storage>) noexcept;
    friend class Builder;
};

struct BlockResult {
    Error error{Error::None};
    BlockRef block{};
    explicit operator bool() const noexcept { return error == Error::None; }
};
struct AppendResult {
    Error error{Error::None};
    NodeId node{};
    std::uint64_t value_unit{}; // Zero for Void/failure; node still names every successful birth.
    constexpr Value GetValue() const noexcept {
        return value_unit ? Value{value_unit, node} : Value{};
    }
    explicit operator bool() const noexcept { return error == Error::None; }
};
static_assert(sizeof(AppendResult) == 16);
struct SubmitResult {
    Error error{Error::None};
    Program program{};
    explicit operator bool() const noexcept { return error == Error::None; }
};

}  // namespace swift::runtime::ir
