#include "allocate.h"

#include <algorithm>
#include <array>
#include <limits>

namespace swift::runtime::backend::riscv64 {
namespace {
using namespace ir;
using u32 = std::uint32_t;

constexpr unsigned first_register = 18;  // s2..s10; s1 is state and s11 is flags.
constexpr unsigned register_count = 9;

bool SupportedType(Type type) {
    return type == Type::Void || IntegerType(type) || type == Type::F32 || type == Type::F64 ||
           type == Type::V128;
}

bool UnsupportedStorage(OpCode opcode) {
    return opcode == OpCode::AddPhi || opcode == OpCode::DefineLocal ||
           opcode == OpCode::LoadLocal || opcode == OpCode::StoreLocal;
}

void Allocate(const Program& program, Scope scope, Allocation& result) {
    result.error = CompileError::None;
    result.node = {};
    result.block = {};
    result.spill_bytes = 0;
    result.homes.clear();
    result.block_order.clear();
    const auto reject = [&](CompileError error, NodeId node = {}, BlockId block = {}) {
        result.error = error;
        result.node = node;
        result.block = block;
        result.spill_bytes = 0;
        result.homes.clear();
        result.block_order.clear();
    };
    const auto operations = program.Operations();
    const auto blocks = program.Blocks();
    const auto entry = program.EntryBlock();
    if (!program || program.ScopeKind() != scope || blocks.empty() ||
        entry.value >= blocks.size() || operations.size() >= NodeId::invalid ||
        (scope == Scope::Block && (blocks.size() != 1 || entry.value != 0))) {
        reject(CompileError::InvalidProgram);
        return;
    }

    // Schedule the actual CFG. Disconnected declared entries remain separate
    // components, rather than borrowing the incidental source-body order.
    const bool graph = scope == Scope::Function &&
                       (blocks.size() != 1 || blocks.front().terminal.kind == TerminalKind::Jump ||
                        blocks.front().terminal.kind == TerminalKind::Branch);
    std::vector<u32> rank, starts;
    if (!graph)
        result.block_order.push_back(0);
    else {
        rank.resize(blocks.size());
        starts.resize(blocks.size());

        struct Frame {
            u32 block, edge;
        };

        std::vector<Frame> stack;
        stack.reserve(blocks.size());
        result.block_order.reserve(blocks.size());
        const auto component = [&](u32 root) {
            const auto begin = result.block_order.size();
            rank[root] = 1;
            stack.push_back({root, 0});
            while (!stack.empty()) {
                auto& frame = stack.back();
                const auto& terminal = blocks[frame.block].terminal;
                BlockId next;
                if (terminal.kind == TerminalKind::Jump && frame.edge < 1) {
                    next = terminal.first;
                    ++frame.edge;
                } else if (terminal.kind == TerminalKind::Branch && frame.edge < 2)
                    next = frame.edge++ == 0 ? terminal.first : terminal.second;
                else {
                    rank[frame.block] = 2;
                    result.block_order.push_back(frame.block);
                    stack.pop_back();
                    continue;
                }
                if (!rank[next.value]) {
                    rank[next.value] = 1;
                    stack.push_back({next.value, 0});
                }
            }
            std::reverse(result.block_order.begin() + begin, result.block_order.end());
        };
        component(entry.value);
        for (u32 b = 0; b < blocks.size(); ++b)
            if (!rank[b]) component(b);
        for (u32 i = 0; i < result.block_order.size(); ++i) rank[result.block_order[i]] = i;
    }
    const auto block_start = [&](u32 b) { return graph ? starts[b] : 0u; };
    result.homes.resize(operations.size());
    u32 position{};
    bool backedge{};
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (graph) starts[b] = position;
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i, ++position) {
            const auto& operation = operations[i];
            if (!SupportedType(operation.type)) {
                reject(CompileError::UnsupportedType, NodeId{i}, BlockId{b});
                return;
            }
            if (UnsupportedStorage(operation.opcode)) {
                reject(operation.opcode == OpCode::AddPhi ? CompileError::UnsupportedControlFlow
                                                          : CompileError::UnsupportedOpcode,
                       NodeId{i},
                       BlockId{b});
                return;
            }
            auto& home = result.homes[i];
            home.definition_position = home.last_use = position;
            // Ordinary flag metadata updates the live s11 word. Only an
            // actual named snapshot consumer below needs a saved payload.
            if (operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid)
                home.auxiliary_offset = 0;  // Allocate only after the actual use walk.
        }
        if (graph) {
            const auto edge = [&](BlockId target) { backedge |= rank[target.value] <= rank[b]; };
            if (body.terminal.kind == TerminalKind::Jump) edge(body.terminal.first);
            if (body.terminal.kind == TerminalKind::Branch) {
                edge(body.terminal.first);
                edge(body.terminal.second);
            }
        }
    }

    // A value defined outside a loop survives through the retreating edge
    // even if its lexically last read precedes that edge. The range maximum
    // closes intersecting/irreducible loops without a B-by-N liveness table.
    std::vector<u32> headers, ends;
    std::size_t base{1};
    if (backedge) {
        headers.reserve(blocks.size());
        for (const auto b : result.block_order) headers.push_back(starts[b]);
        headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
        while (base < headers.size()) base *= 2;
        ends.resize(base * 2);
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            const auto add = [&](BlockId target) {
                if (rank[target.value] > rank[b]) return;
                const auto at =
                        base + static_cast<std::size_t>(std::lower_bound(headers.begin(),
                                                                         headers.end(),
                                                                         starts[target.value]) -
                                                        headers.begin());
                ends[at] = std::max(ends[at], starts[b] + body.node_count);
            };
            if (body.terminal.kind == TerminalKind::Jump) add(body.terminal.first);
            if (body.terminal.kind == TerminalKind::Branch) {
                add(body.terminal.first);
                add(body.terminal.second);
            }
        }
        for (auto at = base; at-- > 1;) ends[at] = std::max(ends[at * 2], ends[at * 2 + 1]);
        for (auto i = headers.size(); i-- > 0;) {
            auto end = ends[base + i];
            if (!end) continue;
            auto left = base + i + 1;
            auto right = base + static_cast<std::size_t>(
                                        std::upper_bound(headers.begin(), headers.end(), end) -
                                        headers.begin());
            while (left < right) {
                if (left & 1) end = std::max(end, ends[left++]);
                if (right & 1) end = std::max(end, ends[--right]);
                left /= 2;
                right /= 2;
            }
            auto at = base + i;
            ends[at] = end;
            while ((at /= 2)) ends[at] = std::max(ends[at * 2], ends[at * 2 + 1]);
        }
    }
    const auto loop_end = [&](u32 definition, u32 use) {
        u32 end = use;
        if (ends.empty()) return end;
        auto left = base + static_cast<std::size_t>(
                                   std::upper_bound(headers.begin(), headers.end(), definition) -
                                   headers.begin());
        auto right = base + static_cast<std::size_t>(
                                    std::upper_bound(headers.begin(), headers.end(), end) -
                                    headers.begin());
        while (left < right) {
            if (left & 1) end = std::max(end, ends[left++]);
            if (right & 1) end = std::max(end, ends[--right]);
            left /= 2;
            right /= 2;
        }
        return end;
    };
    const auto use = [&](u32 definition, u32 at, u32 block = BlockId::invalid) {
        if (definition >= operations.size() || operations[definition].type == Type::Void ||
            result.homes[definition].definition_position >= at)
            return false;
        auto& home = result.homes[definition];
        home.last_use = std::max(home.last_use,
                                 operations[definition].block.value == block
                                         ? at
                                         : loop_end(home.definition_position, at));
        return true;
    };
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (body.terminal.HasValue() &&
            !use(body.terminal.value.value, block_start(b) + body.node_count)) {
            reject(CompileError::InvalidProgram, {}, BlockId{b});
            return;
        }
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            const auto& operation = operations[i];
            const bool saved_flags = operation.opcode == OpCode::SaveFlags ||
                                     operation.opcode == OpCode::BranchOnlyFlags;
            for (const auto input : program.Operands(NodeId{i})) {
                if (input.Kind() != OperandKind::Value) continue;
                const auto definition = static_cast<u32>(input.Payload());
                if (!use(definition, result.homes[i].definition_position, b)) {
                    reject(CompileError::InvalidProgram, NodeId{i}, BlockId{b});
                    return;
                }
                if (saved_flags) result.homes[definition].auxiliary_offset = 0;
            }
        }
    }

    const auto reserve = [&](u32 bytes, u32 alignment, u32 node, u32& offset) {
        constexpr auto limit = static_cast<u32>(std::numeric_limits<std::int32_t>::max()) - 16;
        if (result.spill_bytes > limit - bytes - alignment) {
            reject(CompileError::FrameTooLarge, NodeId{node}, operations[node].block);
            return false;
        }
        offset = (result.spill_bytes + alignment - 1) & ~(alignment - 1);
        result.spill_bytes = offset + bytes;
        return true;
    };
    for (u32 i = 0; i < operations.size(); ++i)
        if (result.homes[i].auxiliary_offset != UINT32_MAX &&
            !reserve(16, 16, i, result.homes[i].auxiliary_offset))
            return;

    std::array<u32, register_count> residents;
    residents.fill(NodeId::invalid);
    const auto spill = [&](u32 node) {
        auto& home = result.homes[node];
        const auto bytes = operations[node].type == Type::V128 ? 16u : 8u;
        if (!reserve(bytes, bytes, node, home.stack_offset)) return false;
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
                        distance = std::min(distance, result.homes[residents[at + half]].last_use);
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
            home.reg = static_cast<std::uint8_t>(first_register + chosen);
            if (width == 2) home.high_reg = static_cast<std::uint8_t>(home.reg + 1);
            for (unsigned half = 0; half < width; ++half) residents[chosen + half] = i;
        }
    }
    result.spill_bytes = (result.spill_bytes + 15u) & ~15u;
}
}  // namespace

void AllocateBlock(const ir::Program& program, Allocation& allocation) {
    Allocate(program, ir::Scope::Block, allocation);
}

void AllocateFunction(const ir::Program& program, Allocation& allocation) {
    Allocate(program, ir::Scope::Function, allocation);
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
