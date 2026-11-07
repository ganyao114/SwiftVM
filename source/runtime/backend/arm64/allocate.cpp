#include "allocation_detail.h"

#include <array>
#include <bit>

namespace swift::runtime::backend::arm64::detail {
void ResetAllocation(Allocation& result, CompileError error, NodeId node, BlockId block) noexcept {
    result.error = error;
    result.node = node;
    result.block = block;
    result.spill_bytes = 0;
    result.publish_entry_pc = false;
    result.homes.clear();
    result.block_order.clear();
}

void Allocator::Run() try {
    ResetAllocation(result);
    const auto entry = program.EntryBlock();
    if (!program || program.ScopeKind() != scope || blocks.empty() ||
        entry.value >= blocks.size() ||
        (scope == Scope::Block && (blocks.size() != 1 || entry.value != 0)) ||
        operations.size() >= NodeId::invalid) {
        Reject(CompileError::InvalidProgram);
        return;
    }
    pinned = options && options->pinned_uniform_offset != UINT32_MAX;
    if (pinned &&
        ((options->pinned_uniform_slots != 8 && options->pinned_uniform_slots != 12 &&
          options->pinned_uniform_slots != 16) ||
         options->pinned_uniform_offset > UINT32_MAX - (options->pinned_uniform_slots * 8u - 1))) {
        Reject(CompileError::UnsupportedOperand);
        return;
    }
    vector_pinned = options && options->pinned_vector_offset != UINT32_MAX;
    if (vector_pinned && options->pinned_vector_offset > UINT32_MAX - 255u) {
        Reject(CompileError::UnsupportedOperand);
        return;
    }
    const bool linked = options && options->dispatcher && options->exit_handler;
    base_scalar_registers = pinned ? 14u - (options->pinned_uniform_slots - 8u) : 14u;
    scalar_registers = base_scalar_registers + (linked ? 1u : 0u);
    borrow_pins = (pinned || vector_pinned) && options->private_uniforms;
    raw_flags = options && options->native_nzcv && linked;
    deferred_pc = options && options->deferred_pc;
    graph = scope == Scope::Function &&
            (blocks.size() != 1 || blocks.front().terminal.kind == TerminalKind::Jump ||
             blocks.front().terminal.kind == TerminalKind::Branch);

    if (graph)
        cfg.BuildOrder(program, result.block_order);
    else
        result.block_order.push_back(0);
    if (!InitializeHomes()) return;
    if (graph) cfg.CloseLoops(program, result.block_order);
    if (!SelectHomes()) return;
    AssignRegisters();
} catch (...) {
    ResetAllocation(result, CompileError::InvalidProgram);
    throw;
}

bool Allocator::InitializeHomes() {
    result.homes.resize(operations.size());
    u32 position{};
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (deferred_pc && (body.terminal.kind == TerminalKind::Return ||
                            body.terminal.kind == TerminalKind::Unreachable))
            result.publish_entry_pc = true;
        const auto pc_check = TailPCCondition(b);
        const auto terminal_value =
                body.terminal.kind == TerminalKind::Branch ? body.terminal.value : pc_check;
        if (graph) cfg.RecordBlock(b, position, body.terminal);
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i, ++position) {
            if (operations[i].type != Type::Void && !IntegerType(operations[i].type) &&
                operations[i].type != Type::V128) {
                Reject(CompileError::UnsupportedType, NodeId{i}, BlockId{b});
                return false;
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
                    Reject(CompileError::FrameTooLarge, NodeId{i});
                    return false;
                }
                result.homes[i].extra_slot = static_cast<std::uint16_t>(result.spill_bytes / 8 + 1);
                result.spill_bytes += 8;
            }
        }
    }
    return true;
}

void Allocator::AssignRegisters() {
    std::array<u32, 15> scalar_resident;
    std::array<u32, 20> vector_resident;
    scalar_resident.fill(NodeId::invalid);
    vector_resident.fill(NodeId::invalid);
    const auto spill = [&](u32 node) {
        const auto size = operations[node].type == Type::V128 ? 16u : 8u;
        const auto offset = (result.spill_bytes + size - 1) & ~(size - 1);
        if (offset + size > 32768) {
            Reject(CompileError::FrameTooLarge, NodeId{node}, operations[node].block);
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
                    const unsigned reg = input_home.reg;
                    const auto input_slot = vector ? reg - (reg >= 10 ? 4u : 2u)
                                            : reg == abi::ExtraScalarRegister
                                                    ? base_scalar_registers
                                                    : reg - 2u;
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
            // twenty-register mapping. Only linked scalar banks append x28.
            result.homes[i].reg = static_cast<std::uint8_t>(
                    vector ? abi::VectorHomeRegister(static_cast<unsigned>(slot), vector_pinned)
                           : abi::ScalarHomeRegister(static_cast<unsigned>(slot),
                                                     base_scalar_registers));
            resident[slot] = i;
        }
    }
}
}  // namespace swift::runtime::backend::arm64::detail

namespace swift::runtime::backend::arm64 {
void AllocateBlock(const ir::Program& program,
                   Allocation& allocation,
                   const CompileOptions* options) {
    detail::Allocator(program, ir::Scope::Block, options, allocation).Run();
}

void AllocateFunction(const ir::Program& program,
                      Allocation& allocation,
                      const CompileOptions* options) {
    detail::Allocator(program, ir::Scope::Function, options, allocation).Run();
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
