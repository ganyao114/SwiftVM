#include "allocate.h"
#include "helpers.h"
#include "runtime/backend/allocation_cfg.h"

#include <algorithm>
#include <array>
#include <limits>

namespace swift::runtime::backend::x64 {
namespace {
using namespace ir;
using u32 = std::uint32_t;

constexpr std::array<unsigned, 6> scalar_registers{13, 14, 15, 5, 8, 9};
constexpr unsigned register_count = 12;

bool SupportedType(Type type) {
    return type == ir::Type::Void || IntegerType(type) || type == ir::Type::F32 ||
           type == ir::Type::F64 || type == ir::Type::V128;
}

bool UnsupportedStorage(OpCode opcode) {
    return opcode == OpCode::AddPhi || opcode == OpCode::DefineLocal ||
           opcode == OpCode::LoadLocal || opcode == OpCode::StoreLocal;
}

// The private owner only borrows the original request and its actual result.
// Control flow is shared; register and auxiliary-slot policy stays here.
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
        result.spill_bytes = result.call_offset = 0;
        result.has_calls = false;
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
                if (operation.opcode == OpCode::CompareAndSwap128 ||
                    operation.opcode == OpCode::CallLambda || operation.opcode == OpCode::Cpuid ||
                    operation.opcode == OpCode::ReadSystem ||
                    operation.opcode == OpCode::ReadSystemPair ||
                    operation.opcode == OpCode::X87Op || operation.opcode == OpCode::X87Execute ||
                    operation.opcode == OpCode::Div128 ||
                    operation.opcode == OpCode::VecFRoundInt ||
                    operation.opcode == OpCode::VecFMulAdd ||
                    operation.opcode == OpCode::Sse42Str ||
                    operation.opcode == OpCode::Sse42StrExplicit ||
                    operation.opcode == OpCode::VecAesEncFast ||
                    operation.opcode == OpCode::VecAesDecFast ||
                    operation.opcode == OpCode::VecAesEncLastFast ||
                    operation.opcode == OpCode::VecAesDecLastFast ||
                    operation.opcode == OpCode::VecAesImc ||
                    operation.opcode == OpCode::VecAesEnc ||
                    operation.opcode == OpCode::VecAesDec ||
                    operation.opcode == OpCode::VecAesEncLast ||
                    operation.opcode == OpCode::VecAesDecLast ||
                    operation.opcode == OpCode::VecAesKeygenAssist ||
                    operation.opcode == OpCode::VecPclMul ||
                    operation.opcode == OpCode::VecSha256Msg1 ||
                    operation.opcode == OpCode::VecSha256Msg2 ||
                    operation.opcode == OpCode::VecSha256Rnds2)
                    result.has_calls = true;
                if (operation.opcode == OpCode::VecFCmpMask) {
                    unsigned argument{};
                    for (const auto operand : program.Operands(NodeId{i})) {
                        if (argument++ == 3) {
                            result.has_calls |= FloatingCompareNeedsCall(operand.Payload());
                            break;
                        }
                    }
                }
                auto& home = result.homes[i];
                home.definition_position = home.last_use = position;
                // Ordinary flag metadata updates the live RBX word. Only an
                // actual named snapshot consumer below needs a saved payload.
                if (operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid)
                    home.auxiliary_offset = 0;  // Allocate only after the actual use walk.
            }
        }
        return true;
    }

    bool RecordUse(u32 definition, u32 at, u32 block = BlockId::invalid) {
        if (definition >= operations.size() || operations[definition].type == ir::Type::Void ||
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
        std::array<std::array<u32, register_count>, 2> banks;
        for (auto& bank : banks) bank.fill(NodeId::invalid);
        const auto spill = [&](u32 id, auto& residents) {
            auto& h = result.homes[id];
            unsigned bytes = operations[id].type == ir::Type::V128 ? 16 : 8;
            if (!Reserve(bytes, bytes, id, h.stack_offset)) return false;
            h.spilled = true;
            for (auto& r : residents)
                if (r == id) r = NodeId::invalid;
            return true;
        };
        for (auto b : result.block_order) {
            const auto& body = blocks[b];
            for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
                if (operations[i].type == ir::Type::Void) continue;
                const bool vector = operations[i].type == ir::Type::V128;
                auto& bank = banks[vector];
                const unsigned count = vector ? 12 : scalar_registers.size();
                auto& h = result.homes[i];
                unsigned chosen = count;
                u32 far = h.last_use;
                for (unsigned j = 0; j < count; ++j) {
                    if (bank[j] != NodeId::invalid &&
                        result.homes[bank[j]].last_use < h.definition_position)
                        bank[j] = NodeId::invalid;
                    if (bank[j] == NodeId::invalid) {
                        chosen = j;
                        break;
                    }
                    if (result.homes[bank[j]].last_use > far) {
                        far = result.homes[bank[j]].last_use;
                        chosen = j;
                    }
                }
                if (chosen == count) {
                    if (!spill(i, bank)) return;
                    continue;
                }
                if (bank[chosen] != NodeId::invalid && !spill(bank[chosen], bank)) return;
                h.reg = vector ? 2 + chosen : scalar_registers[chosen];
                bank[chosen] = i;
            }
        }
        result.spill_bytes = (result.spill_bytes + 15) & ~15u;
        result.call_offset = result.spill_bytes;
        if (result.has_calls) result.spill_bytes += 320;
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

}  // namespace swift::runtime::backend::x64
