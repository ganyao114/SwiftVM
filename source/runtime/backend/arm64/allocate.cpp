#include "allocate.h"
#include "abi.h"
#include "compile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace swift::runtime::backend::arm64 {
namespace {
using namespace ir;
using u32 = std::uint32_t;

constexpr bool ReadsBeforeWrite(OpCode opcode, bool vector = false) {
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
            case OpCode::VecFCmpMask:
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

void Allocate(const Program& program,
              Scope scope,
              const CompileOptions* options,
              Allocation& result) try {
    result.error = CompileError::None;
    result.node = {};
    result.block = {};
    result.spill_bytes = 0;
    result.publish_entry_pc = false;
    result.homes.clear();
    result.block_order.clear();
    const auto reject = [&](CompileError error, NodeId node = {}, BlockId block = {}) {
        result.error = error;
        result.node = node;
        result.block = block;
        result.spill_bytes = 0;
        result.publish_entry_pc = false;
        result.homes.clear();
        result.block_order.clear();
    };
    const auto operations = program.Operations();
    const auto blocks = program.Blocks();
    const auto entry = program.EntryBlock();
    if (!program || program.ScopeKind() != scope || blocks.empty() ||
        entry.value >= blocks.size() ||
        (scope == Scope::Block && (blocks.size() != 1 || entry.value != 0)) ||
        operations.size() >= NodeId::invalid) {
        reject(CompileError::InvalidProgram);
        return;
    }
    const bool pinned = options && options->pinned_uniform_offset != UINT32_MAX;
    if (pinned &&
        ((options->pinned_uniform_slots != 8 && options->pinned_uniform_slots != 12 &&
          options->pinned_uniform_slots != 16) ||
         options->pinned_uniform_offset > UINT32_MAX - (options->pinned_uniform_slots * 8u - 1))) {
        reject(CompileError::UnsupportedOperand);
        return;
    }
    const bool vector_pinned = options && options->pinned_vector_offset != UINT32_MAX;
    if (vector_pinned && options->pinned_vector_offset > UINT32_MAX - 255u) {
        reject(CompileError::UnsupportedOperand);
        return;
    }
    const unsigned scalar_registers = pinned ? 14u - (options->pinned_uniform_slots - 8u) : 14u;
    const bool borrow_pins = (pinned || vector_pinned) && options->private_uniforms;
    const bool raw_flags =
            options && options->native_nzcv && options->dispatcher && options->exit_handler;
    const bool deferred_pc = options && options->deferred_pc;
    // One fixed key space for actual Uniform slots, not another value table.
    // Scalar aliases of XMM fields are handled as byte-range accesses below.
    const auto uniform_slot = [&](CellView uniform) {
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
    };
    const auto pin_register = [&](unsigned slot) {
        return slot < 16 ? abi::PinnedUniformRegister(slot, options->pinned_uniform_slots)
                         : abi::PinnedVectorRegister(slot - 16);
    };
    // Program storage is private and Builder enforces block extents/ownership,
    // closed terminals and in-unit targets before Submit. Consume that contract
    // here instead of repeating a whole-program structural validation pass.

    const bool graph = scope == Scope::Function &&
                       (blocks.size() != 1 || blocks.front().terminal.kind == TerminalKind::Jump ||
                        blocks.front().terminal.kind == TerminalKind::Branch);
    // Schedule the actual CFG, never the incidental order in which bodies
    // were constructed. The DFS reads terminals directly, without an edge
    // mirror. A single external exit needs no graph, even in Function scope.
    // The B-sized state is reused as the final block rank.
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
                } else if (terminal.kind == TerminalKind::Branch && frame.edge < 2) {
                    next = frame.edge++ == 0 ? terminal.first : terminal.second;
                } else {
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
    const auto pc_tail_check = [&](u32 b) -> NodeId {
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
    };

    result.homes.resize(operations.size());
    u32 position{};
    bool backedge{};
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (deferred_pc && (body.terminal.kind == TerminalKind::Return ||
                            body.terminal.kind == TerminalKind::Unreachable))
            result.publish_entry_pc = true;
        const auto pc_check = pc_tail_check(b);
        const auto terminal_value =
                body.terminal.kind == TerminalKind::Branch ? body.terminal.value : pc_check;
        if (graph) starts[b] = position;
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i, ++position) {
            if (operations[i].type != Type::Void && !IntegerType(operations[i].type) &&
                operations[i].type != Type::V128) {
                reject(CompileError::UnsupportedType, NodeId{i}, BlockId{b});
                return;
            }
            result.homes[i].definition_position = position;
            result.homes[i].last_use = position;
            if (i == terminal_value.value && operations[i].Metadata() == OperationMetadata{}) {
                const auto op = operations[i].opcode;
                if (op == OpCode::CondSet || op == OpCode::LocalCondSet)
                    result.homes[i].terminal_only = true;
                else if (op == OpCode::TestFlags || op == OpCode::TestNotFlags) {
                    const auto mask = program.Operands(NodeId{i}).front().Payload();
                    constexpr auto supported = FlagBits(Flag::Carry | Flag::Overflow | Flag::Zero |
                                                        Flag::Negate | Flag::AuxiliaryCarry);
                    result.homes[i].terminal_only =
                            (mask && !(mask & (mask - 1)) && !(mask & ~supported)) ||
                            (options && options->native_nzcv &&
                             mask == FlagBits(Flag::Carry | Flag::Zero));
                }
            } else if (terminal_value && i > terminal_value.value &&
                       result.homes[terminal_value.value].terminal_only) {
                // The final PC selection is the flags reader itself. Every
                // other architectural writer still ends a delayed read;
                // pure compares preserve the guest flags in the emitter.
                constexpr auto barriers = Effect::WriteFlags | Effect::WriteGuestState |
                                          Effect::WriteMemory | Effect::Unknown | Effect::External |
                                          Effect::Control;
                if (!(pc_check && i + 1 == body.first_node + body.node_count) &&
                    HasAnyEffect(EffectsFor(operations[i].opcode, operations[i].Metadata()),
                                 barriers))
                    result.homes[terminal_value.value].terminal_only = false;
            }
            if (operations[i].opcode == OpCode::Div128 || operations[i].opcode == OpCode::Cpuid) {
                if (result.spill_bytes == 32768) {
                    reject(CompileError::FrameTooLarge, NodeId{i});
                    return;
                }
                result.homes[i].extra_slot = static_cast<std::uint16_t>(result.spill_bytes / 8 + 1);
                result.spill_bytes += 8;
            }
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

    // A single B-sized range maximum exists only when a real backedge exists.
    // A header at/before the definition requires reexecuting that definition
    // before another use under the producer's SSA dominance contract. This
    // also covers empty blocks at equal positions in the actual RPO schedule.
    std::vector<u32> headers, ends;
    std::size_t base{1};
    if (backedge) {
        headers.reserve(blocks.size());
        for (const auto b : result.block_order) headers.push_back(starts[b]);
        // Scheduled starts are already nondecreasing, including empty blocks.
        headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
        while (base < headers.size()) base *= 2;
        ends.resize(base * 2);
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            const auto add = [&](BlockId target) {
                // Rank distinguishes true retreating edges even where empty
                // blocks share their scheduled node coordinate.
                if (rank[target.value] > rank[b]) return;
                const auto h = starts[target.value];
                const auto at = base + static_cast<std::size_t>(
                                               std::lower_bound(headers.begin(), headers.end(), h) -
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
        // Crossing/irreducible backedges need transitive interval closure:
        // entering a second header before the first source end can carry a
        // still-needed value beyond that end. Right-hand headers are already
        // closed, so one right-to-left range query per header suffices.
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
    const auto use = [&](u32 definition, u32 at, u32 node_block = BlockId::invalid) {
        if (definition >= operations.size() || operations[definition].type == Type::Void ||
            result.homes[definition].definition_position >= at)
            return false;
        // No block start lies between a same-block definition and ordinary
        // node use. Terminals omit node_block: their end coordinate can be
        // another (possibly empty) loop header and still needs loop_end.
        const auto end = operations[definition].block.value == node_block
                                 ? at
                                 : loop_end(result.homes[definition].definition_position, at);
        result.homes[definition].last_use = std::max(result.homes[definition].last_use, end);
        return true;
    };
    for (auto order = result.block_order.size(); order-- > 0;) {
        const auto b = result.block_order[order];
        const auto& body = blocks[b];
        const auto pc_check = pc_tail_check(b);
        const auto block_end = block_start(b) + body.node_count;
        std::uint64_t needed_nzcv = FlagBits(Flag::NZCV);
        if (body.terminal.HasValue() && !use(body.terminal.value.value, block_end)) {
            reject(CompileError::InvalidProgram, {}, BlockId{b});
            return;
        }
        if (body.terminal.HasValue() && operations[body.terminal.value.value].block.value != b)
            result.homes[body.terminal.value.value].terminal_only = false;

        // Reuse the actual use walk: later uses and writes are known before
        // each definition. A borrowed pin never escapes this one block.
        std::array<u32, 32> next_pinned_write;
        if (borrow_pins) next_pinned_write.fill(block_end);
        std::array<u32, 32> pending;
        u32 pending_mask{};
        const auto cancel_pending = [&](unsigned slot) {
            const auto bit = 1u << slot;
            if (!(pending_mask & bit)) return;
            result.homes[pending[slot]].reg = 0;
            pending_mask &= ~bit;
        };
        const auto detach_pending = [&](u32 producer) {
            const auto reg = result.homes[producer].reg;
            if (!pending_mask || !reg) return 32u;
            const auto slot = operations[producer].type == Type::V128
                                      ? 16u + abi::PinnedVectorSlot(reg)
                                      : abi::PinnedUniformSlot(reg, options->pinned_uniform_slots);
            const auto bit = 1u << slot;
            if (!(pending_mask & bit) || pending[slot] != producer) return 32u;
            pending_mask &= ~bit;
            return slot;
        };
        const auto uniform_access = [&](CellView uniform, u32 at, bool write) {
            if (!write && !pending_mask) return;
            const auto offset = uniform.Payload();
            const auto bytes = TypeBits(uniform.TypeView()) / 8;
            if (!bytes) return;
            for (unsigned bank = 0; bank < 2; ++bank) {
                if (bank ? !vector_pinned : !pinned) continue;
                const std::uint64_t begin =
                        bank ? options->pinned_vector_offset : options->pinned_uniform_offset;
                const unsigned stride = bank ? 16u : 8u;
                const auto end = begin + (bank ? 16u : options->pinned_uniform_slots) * stride;
                if (offset >= end || offset + bytes <= begin) continue;
                const auto first = (std::max(offset, begin) - begin) / stride;
                const auto last = (std::min(offset + bytes, end) - 1 - begin) / stride;
                for (auto local = first; local <= last; ++local) {
                    const auto slot = bank * 16u + static_cast<unsigned>(local);
                    cancel_pending(slot);
                    if (write) next_pinned_write[slot] = at;
                }
            }
        };
        const auto before_write = [&](const Home& home, u32 next, unsigned pin, bool vector) {
            if (home.last_use >= block_end) return false;
            if (home.last_use < next) return true;
            if (home.last_use != next) return false;
            const auto writer = body.first_node + next - block_start(b);
            return result.homes[writer].reg == pin &&
                   (operations[writer].type == Type::V128) == vector &&
                   ReadsBeforeWrite(operations[writer].opcode, vector);
        };
        for (auto i = body.first_node + body.node_count; i-- > body.first_node;) {
            auto& home = result.homes[i];
            const auto& operation = operations[i];
            const auto at = home.definition_position;
            const auto inputs = program.Operands(NodeId{i});
            const auto metadata = operation.Metadata();
            const auto effects = borrow_pins || raw_flags || deferred_pc
                                         ? EffectsFor(operation.opcode, metadata)
                                         : Effect::None;
            const bool uniform_access_op = operation.opcode == OpCode::LoadUniform ||
                                           operation.opcode == OpCode::StoreUniform ||
                                           operation.opcode == OpCode::StoreUniformSelectImm;
            const bool pc_tail = deferred_pc && i + 1 == body.first_node + body.node_count &&
                                 body.terminal.kind == TerminalKind::Dispatch &&
                                 (operation.opcode == OpCode::StoreUniform ||
                                  operation.opcode == OpCode::StoreUniformSelectImm) &&
                                 inputs.front().TypeView() == Type::U64 &&
                                 inputs.front().Payload() == options->pc_offset;
            if (deferred_pc && !result.publish_entry_pc) {
                result.publish_entry_pc = operation.opcode == OpCode::Div128 ||
                                          HasAnyEffect(effects, Effect::Unknown | Effect::External);
                if (uniform_access_op) {
                    const auto field = inputs.front();
                    const auto bytes = TypeBits(field.TypeView()) / 8;
                    if (!pc_tail && field.Payload() < std::uint64_t{options->pc_offset} + 8 &&
                        options->pc_offset < field.Payload() + bytes)
                        result.publish_entry_pc = true;
                } else if (HasAnyEffect(effects, Effect::ReadGuestState | Effect::WriteGuestState))
                    result.publish_entry_pc = true;
            }
            NodeId pc_literal;
            if (pc_tail && operation.opcode == OpCode::StoreUniform) {
                auto input = inputs.begin();
                ++input;
                const auto value = *input;
                if (value.Kind() == OperandKind::Value && value.Payload() < i &&
                    (value.TypeView() == Type::Void || value.TypeView() == Type::U64)) {
                    const auto& producer = operations[value.Payload()];
                    if (producer.opcode == OpCode::LoadImm && producer.type == Type::U64 &&
                        producer.Metadata() == OperationMetadata{} && producer.block.value == b)
                        pc_literal = NodeId{static_cast<u32>(value.Payload())};
                }
            }
            for (const auto cell : inputs)
                if (cell.Kind() == OperandKind::Value) {
                    const bool sole_pc_literal =
                            pc_literal && cell.Payload() == pc_literal.value &&
                            result.homes[cell.Payload()].last_use ==
                                    result.homes[cell.Payload()].definition_position;
                    if (cell.Payload() >= NodeId::invalid ||
                        !use(static_cast<u32>(cell.Payload()), at, b)) {
                        reject(CompileError::InvalidProgram, NodeId{i}, BlockId{b});
                        return;
                    }
                    // Keep the real lifetime use. Only the final PC consumer
                    // may consume this predicate/literal without a scalar home.
                    if (sole_pc_literal)
                        result.homes[cell.Payload()].terminal_only = true;
                    else if (!(pc_check.value == cell.Payload() &&
                               i + 1 == body.first_node + body.node_count))
                        result.homes[cell.Payload()].terminal_only = false;
                }
            if (raw_flags) {
                // Literal metadata already describes definite writes. Keep
                // the unclassified writers/real observers conservative,
                // without introducing another flags opcode classification.
                // This IR permission makes omitted flags' post-state
                // unspecified; it is not a cached liveness conclusion.
                const auto discarded = metadata.discard_omitted_flags
                                               ? FlagBits(Flag::NZCV) & ~metadata.flag_outputs
                                               : 0;
                home.nzcv_clobber = static_cast<std::uint8_t>(
                        (FlagBits(Flag::NZCV) & ~needed_nzcv) | discarded);
                needed_nzcv &= ~metadata.flag_outputs;
                bool observed =
                        HasAnyEffect(effects,
                                     Effect::ReadFlags | Effect::MayFault | Effect::Unknown |
                                             Effect::External | Effect::Control) ||
                        (metadata == OperationMetadata{} &&
                         HasAnyEffect(effects, Effect::WriteFlags));
                if (HasAnyEffect(effects, Effect::ReadGuestState | Effect::WriteGuestState)) {
                    if (operation.opcode != OpCode::LoadUniform &&
                        operation.opcode != OpCode::StoreUniform &&
                        operation.opcode != OpCode::StoreUniformSelectImm)
                        observed = true;
                    else {
                        const auto uniform = inputs.front();
                        const auto bytes = TypeBits(uniform.TypeView()) / 8;
                        observed |= uniform.Payload() < std::uint64_t{options->flags_offset} + 8 &&
                                    options->flags_offset < uniform.Payload() + bytes;
                    }
                }
                if (observed) needed_nzcv = FlagBits(Flag::NZCV);
            }
            if (pending_mask) {
                // Only now is an unobserved delayed publication an actual
                // producer write. Earlier borrow origins have not been read.
                const auto confirmed = detach_pending(i);
                if (confirmed != 32) next_pinned_write[confirmed] = at;
            }
            if (pending_mask) {
                const bool observer = HasAnyEffect(effects,
                                                   Effect::MayFault | Effect::Unknown |
                                                           Effect::External | Effect::Control) ||
                                      operation.opcode == OpCode::UniformBarrier ||
                                      operation.opcode == OpCode::XchgBarrier ||
                                      (HasAnyEffect(effects, Effect::ReadGuestState) &&
                                       operation.opcode != OpCode::LoadUniform) ||
                                      (HasAnyEffect(effects, Effect::WriteGuestState) &&
                                       operation.opcode != OpCode::StoreUniform &&
                                       operation.opcode != OpCode::StoreUniformSelectImm);
                if (observer) {
                    while (pending_mask) cancel_pending(std::countr_zero(pending_mask));
                } else if (operation.opcode == OpCode::LoadUniform)
                    uniform_access(inputs.front(), at, false);
            }
            if ((pinned || vector_pinned) && operation.opcode == OpCode::StoreUniform) {
                auto input = inputs.begin();
                const auto uniform = *input++, value = *input;
                const auto slot = uniform_slot(uniform);
                const bool full_slot = slot != 32 && value.Kind() == OperandKind::Value &&
                                       uniform.TypeView() == (slot < 16 ? Type::U64 : Type::V128);
                const bool vector = slot >= 16;
                const auto pin = full_slot ? pin_register(slot) : 0u;
                const auto later_write =
                        full_slot && borrow_pins ? next_pinned_write[slot] : block_end;
                // A provisional home never changes the borrow cursor. Reads
                // can cancel it without repairing any earlier borrow decision.
                if (borrow_pins) uniform_access(uniform, at, true);
                if (full_slot) {
                    const auto definition = static_cast<u32>(value.Payload());
                    const auto& producer = operations[definition];
                    auto& produced = result.homes[definition];
                    const auto view =
                            value.TypeView() == Type::Void ? producer.type : value.TypeView();
                    const bool full_value =
                            vector ? producer.type == Type::V128 && view == Type::V128
                                   : (producer.type == Type::U8 || producer.type == Type::U16 ||
                                      producer.type == Type::U32 || producer.type == Type::U64) &&
                                             producer.opcode != OpCode::BitCast &&
                                             producer.opcode != OpCode::GetResult &&
                                             producer.opcode != OpCode::GetFlags &&
                                             IntegerType(view) &&
                                             TypeBits(view) >= TypeBits(producer.type);
                    if (full_value && !producer.Metadata().branch_only) {
                        // One publication window, including the zero-length
                        // window after an adjacent producer. Later uses must
                        // finish before B ends and no later than a proven
                        // read-before-write replacement of this actual pin.
                        const bool immediate = i > body.first_node && definition == i - 1;
                        const bool lifetime =
                                produced.last_use == at ||
                                (borrow_pins && before_write(produced, later_write, pin, vector));
                        if (lifetime &&
                            (immediate ||
                             (borrow_pins && !produced.reg && producer.block.value == b &&
                              !HasAnyEffect(
                                      EffectsFor(producer.opcode, producer.Metadata()),
                                      Effect::Unknown | Effect::External | Effect::Control)))) {
                            if (pending_mask) (void)detach_pending(definition);
                            produced.reg = static_cast<std::uint8_t>(pin);
                            produced.narrow_pin =
                                    0;  // Full publication must really normalize the carrier.
                            if (immediate) {
                                if (borrow_pins)
                                    next_pinned_write[slot] = produced.definition_position;
                            } else {
                                // Do not hoist the actual write until the
                                // producer is reached without an observer.
                                pending[slot] = definition;
                                pending_mask |= 1u << slot;
                            }
                        }
                    }
                }
            } else if (borrow_pins) {
                if (operation.opcode == OpCode::StoreUniformSelectImm)
                    uniform_access(inputs.front(), at, true);
                else if (HasAnyEffect(effects,
                                      Effect::Unknown | Effect::External | Effect::WriteGuestState))
                    next_pinned_write.fill(at);
            }
            if (borrow_pins && !home.reg && operation.opcode == OpCode::LoadUniform &&
                home.last_use < block_end) {
                const auto uniform = inputs.front();
                const auto slot = uniform_slot(uniform);
                const bool vector = operation.type == Type::V128;
                const bool supported = vector || operation.type == Type::U8 ||
                                       operation.type == Type::U16 || operation.type == Type::U32 ||
                                       operation.type == Type::U64;
                if (slot != 32 && (slot >= 16) == vector && supported &&
                    uniform.TypeView() == operation.type) {
                    const auto pin = pin_register(slot);
                    if (before_write(home, next_pinned_write[slot], pin, vector)) {
                        home.reg = static_cast<std::uint8_t>(pin);
                        home.narrow_pin = operation.type == Type::U8    ? 1
                                          : operation.type == Type::U16 ? 2
                                          : operation.type == Type::U32 ? 3
                                                                        : 0;
                    }
                }
            }
        }
    }

    std::array<u32, 14> scalar_resident;
    std::array<u32, 20> vector_resident;
    scalar_resident.fill(NodeId::invalid);
    vector_resident.fill(NodeId::invalid);
    const auto spill = [&](u32 node) {
        const auto size = operations[node].type == Type::V128 ? 16u : 8u;
        const auto offset = (result.spill_bytes + size - 1) & ~(size - 1);
        if (offset + size > 32768) {
            reject(CompileError::FrameTooLarge, NodeId{node}, operations[node].block);
            return false;
        }
        auto& home = result.homes[node];
        home.spilled = true;
        home.reg = 0;
        home.stack_offset = offset;
        result.spill_bytes = offset + size;
        return true;
    };
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            if (operations[i].type == Type::Void || result.homes[i].terminal_only ||
                result.homes[i].reg)
                continue;
            const auto outputs = operations[i].Metadata().flag_outputs;
            if (raw_flags && IntegerType(operations[i].type) &&
                ReadsBeforeWrite(operations[i].opcode) &&
                (outputs & FlagBits(Flag::Parity | Flag::AuxiliaryCarry)) ==
                        FlagBits(Flag::Parity | Flag::AuxiliaryCarry) &&
                result.homes[i].last_use == result.homes[i].definition_position) {
                // No SSA reader survives this result. The actual result can
                // be the raw PF carrier; AF is independently defined here.
                result.homes[i].reg = 1;
                continue;
            }
            const bool vector = operations[i].type == Type::V128;
            auto resident = vector ? std::span<u32>{vector_resident}.first(vector_pinned ? 12 : 20)
                                   : std::span<u32>{scalar_resident}.first(scalar_registers);
            std::size_t slot{};
            for (; slot < resident.size(); ++slot)
                if (resident[slot] == NodeId::invalid ||
                    result.homes[resident[slot]].last_use < result.homes[i].definition_position)
                    break;
            if (slot == resident.size() &&
                ((vector && vector_pinned) || operations[i].type == Type::U32 ||
                 operations[i].type == Type::U64) &&
                ReadsBeforeWrite(operations[i].opcode, vector)) {
                const auto operands = program.Operands(NodeId{i});
                auto input = operands.begin();
                const auto reuse = [&](CellView value) {
                    if (value.Kind() != OperandKind::Value) return;
                    const auto input_id = static_cast<u32>(value.Payload());
                    const auto& input_home = result.homes[input_id];
                    const auto input_slot = vector && input_home.reg >= 10
                                                    ? static_cast<unsigned>(input_home.reg) - 4u
                                                    : static_cast<unsigned>(input_home.reg) - 2u;
                    // The actual input is consumed before this output replaces it.
                    if (input_home.last_use == result.homes[i].definition_position &&
                        input_slot < resident.size() && resident[input_slot] == input_id)
                        slot = input_slot;
                };
                // The whitelist consumes a bare RHS before writing its result
                // too. Never descend into a compound operand's children.
                if (input != operands.end()) {
                    const auto first = *input++;
                    reuse(first);
                    if (first.Kind() == OperandKind::Value && slot == resident.size() &&
                        input != operands.end())
                        reuse(*input);
                }
            }
            if (slot == resident.size()) {
                slot = 0;
                for (std::size_t other = 1; other < resident.size(); ++other)
                    if (result.homes[resident[other]].last_use >
                        result.homes[resident[slot]].last_use)
                        slot = other;
                if (result.homes[resident[slot]].last_use > result.homes[i].last_use) {
                    if (!spill(resident[slot])) return;
                } else {
                    if (!spill(i)) return;
                    continue;
                }
            }
            // The explicit vector ABI reserves v16..v31 and v0/v1/v8/v9;
            // its dynamic bank is v2..v7,v10..v15. The default keeps its old
            // twenty-register mapping. Scalars retain their requested bank.
            result.homes[i].reg = static_cast<std::uint8_t>(
                    vector ? abi::VectorHomeRegister(static_cast<unsigned>(slot), vector_pinned)
                           : slot + 2);
            resident[slot] = i;
        }
    }
    // Dedicated slots keep every value's home invariant across all edges.
    // The initial writer uses the baseline scaled 12-bit SP load/store form.
    return;
} catch (...) {
    result.error = CompileError::InvalidProgram;
    result.node = {};
    result.block = {};
    result.spill_bytes = 0;
    result.publish_entry_pc = false;
    result.homes.clear();
    result.block_order.clear();
    throw;
}
}  // namespace

void AllocateBlock(const ir::Program& program,
                   Allocation& allocation,
                   const CompileOptions* options) {
    Allocate(program, ir::Scope::Block, options, allocation);
}
void AllocateFunction(const ir::Program& program,
                      Allocation& allocation,
                      const CompileOptions* options) {
    Allocate(program, ir::Scope::Function, options, allocation);
}
Allocation AllocateBlock(const ir::Program& program, const CompileOptions* options) {
    Allocation result;
    AllocateBlock(program, result, options);
    return result;
}
Allocation AllocateFunction(const ir::Program& program, const CompileOptions* options) {
    Allocation result;
    AllocateFunction(program, result, options);
    return result;
}

}  // namespace swift::runtime::backend::arm64
