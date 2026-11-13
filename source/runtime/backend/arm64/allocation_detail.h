#pragma once

// Private work for one allocation request. The Program and final Home owner
// stay borrowed; there is no lowered instruction list or second node table.
#include "allocate.h"
#include "compile.h"
#include "abi.h"
#include "runtime/backend/allocation_cfg.h"

#include <algorithm>
#include <array>

namespace swift::runtime::backend::arm64::detail {
using namespace ir;
using u32 = std::uint32_t;

void ResetAllocation(Allocation&,
                     CompileError error = CompileError::None,
                     NodeId node = {},
                     BlockId block = {}) noexcept;

class Allocator {
public:
    Allocator(const Program& program,
              Scope scope,
              const CompileOptions* options,
              Allocation& result)
            : program(program)
            , scope(scope)
            , options(options)
            , result(result)
            , operations(program.Operations())
            , blocks(program.Blocks()) {}

    void Run();

private:
    const Program& program;
    Scope scope;
    const CompileOptions* options;
    Allocation& result;
    std::span<const Operation> operations;
    std::span<const BlockRecord> blocks;
    backend::detail::AllocationCFG cfg;
    bool pinned{}, vector_pinned{}, borrow_pins{}, raw_flags{}, deferred_pc{}, graph{};
    unsigned base_scalar_registers{}, scalar_registers{};

    bool InitializeHomes();
    bool SelectHomes();
    void AssignRegisters();

    struct BlockSelection {
        BlockSelection(u32 block, u32 end, NodeId pc_check)
                : block(block), end(end), pc_check(pc_check) {}

        u32 block, end;
        NodeId pc_check;
        std::uint64_t needed_nzcv{FlagBits(Flag::NZCV)};
        std::array<u32, 32> next_pinned_write, pending;
        u32 pending_mask{};
        bool all_flags_dead{};
    };

    void CancelPending(BlockSelection&, unsigned slot);
    unsigned DetachPending(BlockSelection&, u32 producer);
    void UniformAccess(BlockSelection&, CellView uniform, u32 at, bool write);
    bool BeforeWrite(const BlockSelection&, const Home&, u32 next, unsigned pin, bool vector) const;
    NodeId ObservePC(u32 node, CellRange inputs, Effect, const BlockSelection&);
    bool RecordInputUses(u32 node, CellRange inputs, NodeId pc_literal, const BlockSelection&);
    void ObserveNZCV(u32 node, CellRange inputs, Effect, BlockSelection&);
    void SelectPinnedHome(u32 node, CellRange inputs, Effect, BlockSelection&);

    void Reject(CompileError error, NodeId node = {}, BlockId block = {}) {
        ResetAllocation(result, error, node, block);
    }

    u32 BlockStart(u32 block) const { return cfg.Start(block); }

    static constexpr bool ReadsBeforeWrite(OpCode opcode, bool vector = false) {
        if (vector) {
            switch (opcode) {
                case OpCode::VecAnd:
                case OpCode::VecOr:
                case OpCode::VecXor:
                case OpCode::VecAndNot:
                case OpCode::VecAdd:
                case OpCode::VecSub:
                case OpCode::VecMul:
                case OpCode::VecCmpEq:
                case OpCode::VecCmpGt:
                case OpCode::VecSignedMulWidenLow:
                case OpCode::VecFAdd:
                case OpCode::VecFSub:
                case OpCode::VecFMul:
                case OpCode::VecFDiv:
                case OpCode::VecFAddScalar64:
                case OpCode::VecFSubScalar64:
                case OpCode::VecFMulScalar64:
                case OpCode::VecFDivScalar64:
                case OpCode::VecFAddScalar32:
                case OpCode::VecFSubScalar32:
                case OpCode::VecFMulScalar32:
                case OpCode::VecFDivScalar32:
                case OpCode::VecFCmpMask:
                case OpCode::VecFMinMax:
                case OpCode::VecFUnary:
                case OpCode::VecFRoundInt:
                case OpCode::VecFMulAdd:
                case OpCode::ArmScalarFP:
                case OpCode::ArmScalarFMA:
                    return true;
                default:
                    return false;
            }
        }
        switch (opcode) {
            case OpCode::LoadMemory:
            case OpCode::LoadMemoryTSO:
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Neg:
            case OpCode::And:
            case OpCode::Or:
            case OpCode::Xor:
            case OpCode::AndNot:
            case OpCode::Mul:
            case OpCode::BitInsert:
            case OpCode::CountLeadingZeros32:
            case OpCode::CountLeadingZeros64:
            case OpCode::CountTrailingZeros32:
            case OpCode::CountTrailingZeros64:
                return true;
            default:
                return false;
        }
    }

    unsigned UniformSlot(CellView uniform) const {
        if (uniform.Kind() != OperandKind::Uniform) return 32u;
        const bool vector = uniform.TypeView() == Type::V128;
        if (vector ? !vector_pinned : !pinned) return 32u;
        const std::uint64_t begin =
                vector ? options->pinned_vector_offset : options->pinned_uniform_offset;
        const unsigned stride = vector ? 16u : 8u;
        const unsigned count = vector ? 16u : options->pinned_uniform_slots;
        const auto offset = uniform.Payload();
        if (offset < begin || offset - begin >= count * stride || (offset - begin) % stride)
            return 32u;
        return (vector ? 16u : 0u) + static_cast<unsigned>((offset - begin) / stride);
    }

    unsigned PinRegister(unsigned slot) const {
        return slot < 16 ? abi::PinnedUniformRegister(slot, options->pinned_uniform_slots)
                         : abi::PinnedVectorRegister(slot - 16);
    }

    NodeId TailPCCondition(u32 b) const {
        const auto& body = blocks[b];
        if (!options || !options->dispatcher || !options->exit_handler || !body.node_count ||
            body.terminal.kind != TerminalKind::Dispatch)
            return {};
        const auto tail = body.first_node + body.node_count - 1;
        if (operations[tail].opcode != OpCode::StoreUniformSelectImm ||
            operations[tail].Metadata() != OperationMetadata{})
            return {};
        auto input = program.Operands(NodeId{tail}).begin();
        const auto uniform = *input++, check = *input++, yes = *input++, no = *input;
        if (uniform.TypeView() != Type::U64 || uniform.Payload() != options->pc_offset ||
            yes.TypeView() != Type::U64 || no.TypeView() != Type::U64 ||
            check.Kind() != OperandKind::Value || check.Payload() >= tail ||
            operations[check.Payload()].block.value != b)
            return {};
        return NodeId{static_cast<u32>(check.Payload())};
    }

    bool RecordUse(u32 definition, u32 at, u32 node_block = BlockId::invalid) {
        if (definition >= operations.size() || operations[definition].type == Type::Void ||
            result.homes[definition].definition_position >= at)
            return false;
        // No block start lies between a same-block definition and ordinary
        // node use. Terminals omit node_block: their end coordinate can be
        // another (possibly empty) loop header and still needs loop_end.
        const auto end = operations[definition].block.value == node_block
                                 ? at
                                 : cfg.UseEnd(result.homes[definition].definition_position, at);
        result.homes[definition].last_use = std::max(result.homes[definition].last_use, end);
        return true;
    }
};
}  // namespace swift::runtime::backend::arm64::detail
