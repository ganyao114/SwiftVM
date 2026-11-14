#include "allocate.h"
#include "runtime/backend/allocation_cfg.h"

#include <algorithm>
#include <array>
#include <limits>

namespace swift::runtime::backend::riscv64 {
namespace {
using namespace ir;
using u32 = std::uint32_t;

// s0 and s2..s10 (10 GPRs = 5 V128 pairs); s1 is guest state and s11 is flags.
constexpr std::array<std::uint8_t, 10> kCalleeRegisters{8, 18, 19, 20, 21, 22, 23, 24, 25, 26};
constexpr unsigned register_count = static_cast<unsigned>(kCalleeRegisters.size());

bool SupportedType(Type type) {
    return type == Type::Void || IntegerType(type) || type == Type::F32 || type == Type::F64 ||
           type == Type::V128;
}

bool UnsupportedStorage(OpCode opcode) {
    return opcode == OpCode::AddPhi || opcode == OpCode::DefineLocal ||
           opcode == OpCode::LoadLocal || opcode == OpCode::StoreLocal;
}

// The private owner only borrows the original request and its actual result.
// Control flow is shared; RV register-pair and auxiliary-slot policy stays here.
class Allocator {
public:
    Allocator(const Program& program, Scope scope, Allocation& result)
            : program(program)
            , scope(scope)
            , result(result)
            , operations(program.Operations())
            , blocks(program.Blocks()) {}

    void Run() {
        ResetResult(CompileError::None);
        const auto entry = program.EntryBlock();
        if (!program || program.ScopeKind() != scope || blocks.empty() ||
            entry.value >= blocks.size() || operations.size() >= NodeId::invalid ||
            (scope == Scope::Block && (blocks.size() != 1 || entry.value != 0))) {
            ResetResult(CompileError::InvalidProgram);
            return;
        }

        graph = scope == Scope::Function &&
                (blocks.size() != 1 || blocks.front().terminal.kind == TerminalKind::Jump ||
                 blocks.front().terminal.kind == TerminalKind::Branch);

        if (graph)
            cfg.BuildOrder(program, result.block_order);
        else
            result.block_order.push_back(0);
        if (!InitializeHomes()) return;
        if (graph) cfg.CloseLoops(program, result.block_order);
        if (!ObserveUses() || !AllocatePayloads()) return;
        AssignRegisters();
    }

private:
    const Program& program;
    Scope scope;
    Allocation& result;
    std::span<const Operation> operations;
    std::span<const BlockRecord> blocks;
    backend::detail::AllocationCFG cfg;
    bool graph{};

    void ResetResult(CompileError error, NodeId node = {}, BlockId block = {}) {
        result.error = error;
        result.node = node;
        result.block = block;
        result.spill_bytes = 0;
        result.homes.clear();
        result.block_order.clear();
    }

    bool InitializeHomes() {
        result.homes.resize(operations.size());
        u32 position{};
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            if (graph) cfg.RecordBlock(b, position, body.terminal);
            for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i, ++position) {
                const auto& operation = operations[i];
                if (!SupportedType(operation.type)) {
                    ResetResult(CompileError::UnsupportedType, NodeId{i}, BlockId{b});
                    return false;
                }
                if (UnsupportedStorage(operation.opcode)) {
                    ResetResult(operation.opcode == OpCode::AddPhi
                                        ? CompileError::UnsupportedControlFlow
                                        : CompileError::UnsupportedOpcode,
                                NodeId{i},
                                BlockId{b});
                    return false;
                }
                auto& home = result.homes[i];
                home.definition_position = home.last_use = position;
                // Ordinary flag metadata updates the live s11 word. Only an
                // actual named snapshot consumer below needs a saved payload.
                if (operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid)
                    home.auxiliary_offset = 0;  // Allocate only after the actual use walk.
            }
        }
        return true;
    }

    bool RecordUse(u32 definition, u32 at, u32 block = BlockId::invalid) {
        if (definition >= operations.size() || operations[definition].type == Type::Void ||
            result.homes[definition].definition_position >= at)
            return false;
        auto& home = result.homes[definition];
        home.last_use = std::max(home.last_use,
                                 operations[definition].block.value == block
                                         ? at
                                         : cfg.UseEnd(home.definition_position, at));
        return true;
    }

    bool ObserveUses() {
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            if (body.terminal.HasValue() &&
                !RecordUse(body.terminal.value.value, cfg.Start(b) + body.node_count)) {
                ResetResult(CompileError::InvalidProgram, {}, BlockId{b});
                return false;
            }
            for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
                const auto& operation = operations[i];
                const bool saved_flags = operation.opcode == OpCode::SaveFlags ||
                                         operation.opcode == OpCode::BranchOnlyFlags;
                for (const auto input : program.Operands(NodeId{i})) {
                    if (input.Kind() != OperandKind::Value) continue;
                    const auto definition = static_cast<u32>(input.Payload());
                    if (!RecordUse(definition, result.homes[i].definition_position, b)) {
                        ResetResult(CompileError::InvalidProgram, NodeId{i}, BlockId{b});
                        return false;
                    }
                    if (saved_flags) result.homes[definition].auxiliary_offset = 0;
                }
            }
        }

        return true;
    }

    bool Reserve(u32 bytes, u32 alignment, u32 node, u32& offset) {
        constexpr auto limit = static_cast<u32>(std::numeric_limits<std::int32_t>::max()) - 16;
        if (result.spill_bytes > limit - bytes - alignment) {
            ResetResult(CompileError::FrameTooLarge, NodeId{node}, operations[node].block);
            return false;
        }
        offset = (result.spill_bytes + alignment - 1) & ~(alignment - 1);
        result.spill_bytes = offset + bytes;
        return true;
    }

    bool AllocatePayloads() {
        for (u32 i = 0; i < operations.size(); ++i)
            if (result.homes[i].auxiliary_offset != UINT32_MAX &&
                !Reserve(16, 16, i, result.homes[i].auxiliary_offset))
                return false;

        return true;
    }

    void AssignRegisters() {
        std::array<u32, register_count> residents;
        residents.fill(NodeId::invalid);
        const auto spill = [&](u32 node) {
            auto& home = result.homes[node];
            const auto bytes = operations[node].type == Type::V128 ? 16u : 8u;
            if (!Reserve(bytes, bytes, node, home.stack_offset)) return false;
            home.spilled = true;
            home.reg = home.high_reg = 0;
            for (auto& resident : residents)
                if (resident == node) resident = NodeId::invalid;
            return true;
        };
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
                if (operations[i].type == Type::Void) continue;
                const auto& incoming = result.homes[i];
                for (auto& resident : residents)
                    if (resident != NodeId::invalid &&
                        result.homes[resident].last_use < incoming.definition_position)
                        resident = NodeId::invalid;
                const unsigned width = operations[i].type == Type::V128 ? 2 : 1;
                unsigned chosen = register_count;
                u32 best_distance{};
                for (unsigned at = 0; at + width <= register_count; ++at) {
                    u32 distance = UINT32_MAX;
                    for (unsigned half = 0; half < width; ++half)
                        if (residents[at + half] != NodeId::invalid)
                            distance =
                                    std::min(distance, result.homes[residents[at + half]].last_use);
                    if (distance == UINT32_MAX) {
                        chosen = at;
                        break;
                    }
                    if (distance > incoming.last_use && distance > best_distance) {
                        chosen = at;
                        best_distance = distance;
                    }
                }
                if (chosen == register_count) {
                    if (!spill(i)) return;
                    continue;
                }
                for (unsigned half = 0; half < width; ++half) {
                    const auto resident = residents[chosen + half];
                    if (resident != NodeId::invalid && !spill(resident)) return;
                }
                auto& home = result.homes[i];
                home.reg = kCalleeRegisters[chosen];
                if (width == 2) home.high_reg = kCalleeRegisters[chosen + 1];
                for (unsigned half = 0; half < width; ++half) residents[chosen + half] = i;
            }
        }
        result.spill_bytes = (result.spill_bytes + 15u) & ~15u;
    }
};
}  // namespace

void AllocateBlock(const ir::Program& program, Allocation& allocation) {
    Allocator(program, ir::Scope::Block, allocation).Run();
}

void AllocateFunction(const ir::Program& program, Allocation& allocation) {
    Allocator(program, ir::Scope::Function, allocation).Run();
}

Allocation AllocateBlock(const ir::Program& program) {
    Allocation result;
    AllocateBlock(program, result);
    return result;
}

Allocation AllocateFunction(const ir::Program& program) {
    Allocation result;
    AllocateFunction(program, result);
    return result;
}

}  // namespace swift::runtime::backend::riscv64
