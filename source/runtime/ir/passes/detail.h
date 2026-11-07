#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include "runtime/ir/builder.h"
#include "runtime/ir/optimize.h"

namespace swift::runtime::ir::passes {
using u64 = std::uint64_t;
inline constexpr u64 all_flags = FlagBits(Flag::All);
inline constexpr u64 live_bit = u64{1} << 62, pin_bit = u64{1} << 63, count_mask = live_bit - 1;

inline bool Metadata(const Operation& op) { return op.Metadata() != OperationMetadata{}; }

inline bool Sensitive(OpCode opcode, Effect effects) {
    switch (opcode) {
        case OpCode::GetFlags:
        case OpCode::SaveFlags:
        case OpCode::BranchOnlyFlags:
        case OpCode::PublishFCmpFlags:
        case OpCode::PublishSse42StrFlags:
        case OpCode::FCmpCondSet:
        case OpCode::Div128Remainder:
        case OpCode::CpuidUpper:
        case OpCode::GetResult:
        case OpCode::LocalParitySet:
        case OpCode::BindLabel:
            return true;
        default:
            return HasAnyEffect(effects, Effect::Unknown);
    }
}

inline bool AddUse(std::vector<u64>& rows, std::uint32_t id, bool pin = false) {
    if (id >= rows.size() || (rows[id] & count_mask) == count_mask) return false;
    rows[id] = (rows[id] | live_bit | (pin ? pin_bit : 0)) + 1;
    return true;
}

inline bool DropUse(std::vector<u64>& rows, std::uint32_t id) {
    if (id >= rows.size() || !(rows[id] & count_mask)) return false;
    --rows[id];
    return true;
}

inline u64 Mask(unsigned bits) { return bits == 64 ? ~u64{} : bits ? (u64{1} << bits) - 1 : 0; }

inline Type ReadType(const Operand& cell, std::span<const Operation> operations) {
    return cell.kind == OperandKind::Value && cell.type == Type::Void
                   ? operations[static_cast<std::size_t>(cell.payload)].type
                   : cell.type;
}

inline bool Scalar(Type type) { return IntegerType(type) && TypeBits(type) <= 64; }

inline bool Foldable(const Operation& op) {
    if (!Scalar(op.type)) return false;
    switch (op.opcode) {
        case OpCode::Zero:
        case OpCode::Neg:
        case OpCode::Not:
        case OpCode::TestZero:
        case OpCode::TestNotZero:
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
        case OpCode::ZeroExtend64:
        case OpCode::SignExtend:
        case OpCode::TestBit:
        case OpCode::BitExtract:
        case OpCode::BitInsert:
        case OpCode::LslImm:
        case OpCode::LslValue:
        case OpCode::LsrImm:
        case OpCode::LsrValue:
        case OpCode::AsrImm:
        case OpCode::AsrValue:
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
            return true;
        default:
            return false;
    }
}

// One invocation borrows the actual editor and use rows. Successful edits
// and narrower flag demand only set needs_final_cleanup; no stage resets it.
struct Context {
    Builder& editor;
    std::vector<u64>& rows;
    OptimizeStats& stats;
    const UniformPolicy& policy;
    bool signed_mul_widen_low{};
    bool needs_final_cleanup{};
};

struct FlagBlock {
    std::array<NodeId, 6> exit_writers{};
    std::uint8_t gen{}, kill{}, input{static_cast<std::uint8_t>(all_flags)},
            output{static_cast<std::uint8_t>(all_flags)};
    bool queued{};
};

// The four existing CFG buffers live at their final stack address for the
// whole invocation. Spans select local arrays or their larger vector fallback.
struct FlagGraph {
    bool function{};
    std::array<FlagBlock, 8> small_flags;
    std::array<std::size_t, 9> small_offsets{};
    std::array<std::size_t, 8> small_pending{};
    std::array<BlockId, 16> small_predecessors;
    std::vector<FlagBlock> flag_storage;
    std::vector<std::size_t> offset_storage, pending_storage;
    std::vector<BlockId> predecessor_storage;
    std::span<FlagBlock> flags;
    std::span<std::size_t> predecessor_offsets, pending;
    std::span<BlockId> predecessors;

    void Prepare(std::span<const BlockRecord> blocks, bool function_scope, OptimizeStats& stats);
};

// These are the original bounded walks, not separately scheduled passes.
Error AnalyzeLiveness(Context&, FlagGraph&);
Error RewriteValues(Context&);
Error EliminateDeadCode(Context&, const FlagGraph&);

bool FoldLowByteMask(const Builder&,
                     std::span<const Operation>,
                     NodeId,
                     std::span<Operand>,
                     std::size_t&,
                     OpCode&);
bool FoldExtractShift(const Builder&,
                      std::span<const Operation>,
                      NodeId,
                      const std::vector<u64>&,
                      std::span<Operand>,
                      std::size_t&,
                      OpCode&);
bool FuseSignedMulWidenLow(const Builder&,
                           std::span<const Operation>,
                           NodeId,
                           const std::vector<u64>&,
                           std::span<Operand>,
                           std::size_t&,
                           OpCode&);
bool FoldOrderedLoadAddress(const Builder&,
                            std::span<const Operation>,
                            NodeId,
                            const std::vector<u64>&,
                            std::span<Operand>,
                            std::size_t&);
bool Fold(const Builder&,
          std::span<const Operation>,
          const Operation&,
          std::span<const Operand>,
          u64&);

}  // namespace swift::runtime::ir::passes
