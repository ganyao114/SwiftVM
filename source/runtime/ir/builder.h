#pragma once

#include <initializer_list>
#include <limits>
#include "program.h"

namespace swift::runtime::ir {

// One append-only request. Bodies are built consecutively; targets can name
// declared future blocks, and values can refer to any already born block.
class Builder {
public:
    explicit Builder(Scope, TargetInfo = {});
    // Start a fresh request in the consumed storage, retaining its capacity.
    // All old borrows end; request metadata and the full unit ID are new.
    Builder(Scope, TargetInfo, Program reusable);
    ~Builder();
    Builder(Builder&&) noexcept;
    Builder& operator=(Builder&&) noexcept;
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;

    // Transfer the one immutable owner back without copying or allocating.
    // All previous Program borrows end here; reacquire them after Submit.
    // The returned request permits only local rewrites, not block creation.
    // The optional absolute bound counts actual arena words, including retired
    // ranges. It does not shrink an existing reserve or limit allocator bytes.
    static Builder Edit(
            Program&&,
            std::size_t max_operand_words = std::numeric_limits<std::size_t>::max()) noexcept;

    // Actual records borrowed from this sole owner, with no decoded mirror.
    // Edit ends all old Program borrows. Rewrite/EraseUnused preserve operation
    // and block addresses; Append/CreateBlock/Reserve or ownership transfer can
    // invalidate them. Reacquire operand ranges after structural writes;
    // ReplaceValueUse preserves their positions and logical boundaries.
    std::uint64_t UnitId() const noexcept { return storage ? storage->unit : 0; }
    BlockId EntryBlock() const noexcept { return storage ? storage->entry : BlockId{}; }
    std::span<const Operation> Operations() const noexcept {
        return storage ? std::span<const Operation>{storage->operations}
                       : std::span<const Operation>{};
    }
    CellRange Operands(NodeId local) const noexcept {
        return storage ? storage->Operands(local) : CellRange{};
    }
    std::span<const BlockRecord> Blocks() const noexcept {
        return storage ? std::span<const BlockRecord>{storage->blocks}
                       : std::span<const BlockRecord>{};
    }

    // Operand capacity is a physical word count, not a logical cell count.
    Error Reserve(std::size_t nodes, std::size_t operand_words, std::size_t blocks);
    BlockResult CreateBlock();
    // Set this unit's actual entry, in construction or Edit. The first created
    // block is the compatible default. No CFG/use walk or body/ID reordering.
    Error SetEntry(BlockRef) noexcept;
    Error BeginBlock(BlockRef);
    AppendResult Append(OpCode, Type result, std::span<const Operand>, OperationMetadata = {});
    AppendResult Append(OpCode op,
                        Type result,
                        std::initializer_list<Operand> operands,
                        OperationMetadata metadata = {}) {
        return Append(op, result, {operands.begin(), operands.size()}, metadata);
    }
    // The same declaration and transaction as Append, with its logical
    // signature/result policy fixed by the caller's compile-time opcode.
    template <OpCode op> AppendResult Emit(Type result,
                                           std::span<const Operand> operands,
                                           OperationMetadata metadata = {}) {
        static_assert(static_cast<std::size_t>(op) < static_cast<std::size_t>(OpCode::Count));
        return AppendImpl<op>(op, result, operands, metadata);
    }
    template <OpCode op> AppendResult Emit(Type result,
                                           std::initializer_list<Operand> operands,
                                           OperationMetadata metadata = {}) {
        return Emit<op>(result, {operands.begin(), operands.size()}, metadata);
    }
    Error EndBlock(Terminal);
    // Update only the original record's flag metadata, in either construction
    // or Edit. No operand/use walk, allocation or backend semantic admission.
    // Full u64 output bits are retained, matching Append/Rewrite. Stable IDs,
    // natural type, opcode, body/operand ranges and all capacities are unchanged.
    Error SetMetadata(NodeId local, OperationMetadata) noexcept;
    // Replace one actual Value edge, retaining its exact effective read type.
    // The iterator must belong to this consumer's current operand range; any
    // structural write or owner transfer ends that borrow. This single-word
    // edit neither allocates nor invalidates other operand iterators. The new
    // source must be an earlier, non-Void value in this owner. Semantic copy
    // equivalence and use bookkeeping remain the caller's responsibilities.
    Error ReplaceValueUse(NodeId consumer,
                          CellRange::Iterator edge,
                          Value replacement,
                          Type view = Type::Void) noexcept;
    // Owner-local, type/order preserving replacement only. Every value must
    // predate local (no self/late inputs, including Phi forward references).
    // This operation is exclusive and non-reentrant; input operands remain
    // immutable for the call. It validates/counts once, then encodes once.
    // Old actual ranges are reused when they fit; tails can shrink/extend.
    // Non-tail growth appends and can leave retired arena words: there is no
    // free-list, compaction or promise of reclaiming arbitrary edit history.
    // Effects/equivalence are caller duties; insertion is not provided.
    Error Rewrite(NodeId local, OpCode, Type, std::span<const Operand>, OperationMetadata = {});
    Error Rewrite(NodeId local,
                  OpCode op,
                  Type type,
                  std::initializer_list<Operand> operands,
                  OperationMetadata metadata = {}) {
        return Rewrite(local, op, type, {operands.begin(), operands.size()}, metadata);
    }
    // Only an Edit owner may erase. The caller must already have removed every
    // data, flag and terminal use; this function does not discover global uses.
    // Effects/flag outputs are checked locally. The record becomes Nop/Void,
    // retaining its ID, block and order; its arena range can remain retired.
    Error EraseUnused(NodeId local) noexcept;
    // Explicit qualification/trace observation: O(N + live words + blocks).
    // Never invoked by Append, Rewrite or Submit. Consumed requests yield zero.
    WorkCounts CountWork() const noexcept;
    // Only checks incrementally maintained completeness, then transfers the
    // unique storage. A failed Submit leaves this request available to finish.
    SubmitResult Submit() noexcept;

private:
    explicit Builder(std::unique_ptr<Program::Storage>, std::size_t max_operand_words) noexcept;
    template <bool measure> Error StoreOperand(const Operand&, std::size_t& words);
    template <int known_signature, bool measure> Error AppendArgument(Signature,
                                                                      std::span<const Operand>,
                                                                      std::size_t&,
                                                                      Type&,
                                                                      std::size_t& words,
                                                                      std::uint32_t before);
    template <OpCode known_op, bool measure> Error ProcessArguments(const Schema&,
                                                                    std::span<const Operand>,
                                                                    Type&,
                                                                    std::size_t& words,
                                                                    std::uint32_t before);
    template <OpCode known_op>
    AppendResult AppendImpl(OpCode, Type, std::span<const Operand>, OperationMetadata);
    std::unique_ptr<Program::Storage> storage;
    bool edit_only{};
    std::size_t max_operand_words{std::numeric_limits<std::size_t>::max()};
};

}  // namespace swift::runtime::ir

#include "builder_impl.h"
