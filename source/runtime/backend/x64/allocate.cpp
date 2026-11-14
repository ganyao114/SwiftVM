#include "allocation_detail.h"
#include "runtime/arch/arm64/numeric.h"

#include <algorithm>
#include <limits>

namespace swift::runtime::backend::x64::detail {

void Allocator::Run() {
    ResetResult(CompileError::None);
    const auto entry = program.EntryBlock();
    if (!program || program.ScopeKind() != scope || blocks.empty() ||
        entry.value >= blocks.size() || operations.size() >= NodeId::invalid ||
        (scope == Scope::Block && (blocks.size() != 1 || entry.value != 0))) {
        ResetResult(CompileError::InvalidProgram);
        return;
    }
    if (pinned_slots && pinned_slots != 2) {
        ResetResult(CompileError::UnsupportedOperand);
        return;
    }
    scalar_register_count = 0;
    for (unsigned reg : {13u, 14u, 15u, 5u, 8u, 9u, 6u, 7u}) {
        bool reserved = false;
        for (unsigned i = 0; i < pinned_slots; ++i) reserved |= kPinRegisters[i] == reg;
        if (!reserved) scalar_registers[scalar_register_count++] = reg;
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
    if (options && options->local_flags) AnalyzeFlags();
    AnalyzeUniformBacking();
    AssignRegisters();
}

void Allocator::ResetResult(CompileError error, NodeId node, BlockId block) {
    result.error = error;
    result.node = node;
    result.block = block;
    result.spill_bytes = result.call_offset = 0;
    result.has_calls = false;
    result.uses_fpcr = false;
    result.fpcr_may_change = false;
    result.flags_slot = result.condition_slot = UINT32_MAX;
    result.native_entry.clear();
    result.terminal_conditions.clear();
    result.flags_input.clear();
    result.cold_fp_count = result.cold_event_count = 0;
    result.homes.clear();
    result.block_order.clear();
}

bool Allocator::OperationNeedsCall(const Operation& operation, NodeId id) const {
    if (AlwaysNeedsCall(operation.opcode)) return true;
    if (operation.opcode != OpCode::VecFCmpMask) return false;
    unsigned argument{};
    for (const auto operand : program.Operands(id)) {
        if (argument++ == 3) return FloatingCompareNeedsCall(operand.Payload());
    }
    return false;
}

bool Allocator::CanFoldTerminalCondition(const BlockRecord& body, u32 node) const {
    if (body.terminal.kind != TerminalKind::Branch || body.terminal.value.value != node ||
        node + 1 != body.first_node + body.node_count)
        return false;
    const auto& operation = operations[node];
    if (operation.Metadata() != OperationMetadata{}) return false;
    const auto code = operation.opcode;
    if (code == OpCode::TestFlags || code == OpCode::TestNotFlags) {
        const auto mask = program.Operands(NodeId{node}).front().Payload();
        return !(mask & 16);
    }
    if (!options || !options->local_flags) return false;
    switch (code) {
        case OpCode::CondSet:
        case OpCode::LocalCondSet:
        case OpCode::TestZero:
        case OpCode::TestNotZero:
        case OpCode::TestBit:
            return true;
        default:
            return false;
    }
}

bool Allocator::InitializeHomes() {
    result.homes.resize(operations.size());
    u32 position{};
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (graph) cfg.RecordBlock(b, position, body.terminal);
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i, ++position) {
            const auto& operation = operations[i];
            if (options && options->arm_fpcr != UINT32_MAX) ObserveFPCR(operation, NodeId{i});
            result.cold_fp_count += UsesColdFPStub(operation.opcode);
            result.uses_fpcr |= UsesArmFPCR(operation.opcode);
            result.cold_event_count += IsGuestSafepointOp(operation.opcode);
            if (!SupportedType(operation.type)) {
                ResetResult(CompileError::UnsupportedType, NodeId{i}, BlockId{b});
                return false;
            }
            if (UnsupportedStorage(operation.opcode)) {
                const auto error = operation.opcode == OpCode::AddPhi
                                           ? CompileError::UnsupportedControlFlow
                                           : CompileError::UnsupportedOpcode;
                ResetResult(error, NodeId{i}, BlockId{b});
                return false;
            }
            result.has_calls |= OperationNeedsCall(operation, NodeId{i});
            auto& home = result.homes[i];
            home.definition_position = home.last_use = position;
            home.terminal_only = CanFoldTerminalCondition(body, i);
            // Ordinary flag metadata updates the live RBX word. Only an actual
            // named snapshot consumer below needs a saved payload.
            if (operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid)
                home.auxiliary_offset = 0;  // Allocate only after the actual use walk.
        }
    }
    return true;
}

void Allocator::ObserveFPCR(const Operation& operation, NodeId id) {
    const auto opcode = operation.opcode;
    if (opcode == OpCode::ArmFPBinary || IsArmFPValueOp(opcode)) {
        const bool binary = opcode == OpCode::ArmFPBinary;
        const unsigned argument = binary || opcode == OpCode::ArmFPCompare ? 2
                                  : opcode == OpCode::ArmFPTernary         ? 3
                                                                           : 1;
        auto at = program.Operands(id).begin();
        for (unsigned i = 0; i < argument; ++i) ++at;
        const auto offsets = (*at++).Payload();
        const auto control = static_cast<u32>(offsets);
        const auto status = static_cast<u32>(binary ? (*at).Payload() : offsets >> 32);
        if (!IsImmediateArmFPCR(control))
            result.fpcr_may_change |= Overlaps({control, 4}, {status, 4});
        return;
    }
    if (opcode == OpCode::CallLambda) {
        auto at = program.Operands(id).begin();
        ++at;
        const auto helper = reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric);
        const bool exact_numeric = at != program.Operands(id).end() &&
                                   (*at).Kind() == OperandKind::Immediate &&
                                   (*at).Payload() == helper;
        result.fpcr_may_change |= !exact_numeric;
        return;
    }
    if (opcode == OpCode::Cpuid || opcode == OpCode::ReadSystem ||
        opcode == OpCode::ReadSystemPair || opcode == OpCode::X87Op ||
        opcode == OpCode::X87Execute) {
        result.fpcr_may_change = true;
        return;
    }
    if (opcode == OpCode::StoreUniform || opcode == OpCode::StoreUniformSelectImm) {
        const auto field = program.Operands(id).front();
        const auto bytes = opcode == OpCode::StoreUniformSelectImm
                                   ? 8u
                                   : (TypeBits(field.TypeView()) + 7) / 8;
        result.fpcr_may_change |= options->arm_fpcr_offset == UINT32_MAX ||
                                  (!IsImmediateArmFPCR(options->arm_fpcr_offset) &&
                                   field.Payload() < std::uint64_t{options->arm_fpcr_offset} + 4 &&
                                   options->arm_fpcr_offset < field.Payload() + bytes);
        return;
    }
    if (!options->private_state &&
        HasAnyEffect(EffectsFor(opcode, operation.Metadata()), Effect::WriteMemory)) {
        result.fpcr_may_change = true;
    }
}

bool Allocator::Reserve(u32 bytes, u32 alignment, u32 node, u32& offset) {
    constexpr auto limit = static_cast<u32>(std::numeric_limits<std::int32_t>::max()) - 16;
    if (result.spill_bytes > limit - bytes - alignment) {
        ResetResult(CompileError::FrameTooLarge, NodeId{node}, operations[node].block);
        return false;
    }
    offset = (result.spill_bytes + alignment - 1) & ~(alignment - 1);
    result.spill_bytes = offset + bytes;
    return true;
}

bool Allocator::AllocatePayloads() {
    for (u32 i = 0; i < operations.size(); ++i) {
        if (result.homes[i].auxiliary_offset != UINT32_MAX &&
            !Reserve(16, 16, i, result.homes[i].auxiliary_offset))
            return false;
    }
    return true;
}

void Allocator::ExpireDeadResidents(std::span<u32> bank, u32 position) const {
    for (auto& resident : bank) {
        if (resident != NodeId::invalid && result.homes[resident].last_use < position)
            resident = NodeId::invalid;
    }
}

bool Allocator::TryRematerializeStoreUniformImm(u32 node, const BlockRecord& body) {
    auto& home = result.homes[node];
    if (operations[node].opcode != OpCode::LoadImm || home.uniform_offset != UINT32_MAX ||
        home.auxiliary_offset != UINT32_MAX || node + 1 >= body.first_node + body.node_count ||
        operations[node + 1].opcode != OpCode::StoreUniform ||
        home.last_use != result.homes[node + 1].definition_position)
        return false;
    const auto store_uniform = *program.Operands(NodeId{node + 1}).begin();
    const auto store_bits = TypeBits(store_uniform.TypeView());
    std::uint64_t imm = (*program.Operands(NodeId{node}).begin()).Payload();
    const auto nbits = TypeBits(operations[node].type);
    if (nbits && nbits < 64) imm &= (std::uint64_t{1} << nbits) - 1;
    if (store_uniform.Kind() != OperandKind::Uniform || store_bits < 8 || store_bits > 64 ||
        (store_bits > 32 && imm != std::uint64_t(std::int64_t(static_cast<std::int32_t>(imm)))))
        return false;
    home.rematerialize = true;
    home.spilled = true;
    return true;
}

unsigned Allocator::FindReusableInputSlot(CellView input,
                                          bool vector,
                                          u32 definition_position,
                                          std::span<const u32> bank) const {
    const auto count = static_cast<unsigned>(bank.size());
    if (input.Kind() != OperandKind::Value ||
        (operations[input.Payload()].type == Type::V128) != vector ||
        result.homes[input.Payload()].spilled ||
        result.homes[input.Payload()].last_use != definition_position)
        return count;
    for (unsigned j = 0; j < count; ++j)
        if (bank[j] == input.Payload()) return j;
    return count;
}

unsigned Allocator::TryReuseOperandRegister(u32 node,
                                            bool vector,
                                            std::span<const u32> bank,
                                            bool& reused) {
    const auto count = static_cast<unsigned>(bank.size());
    if (!DestructiveInput(operations[node])) return count;
    const auto def_pos = result.homes[node].definition_position;
    auto inputs = program.Operands(NodeId{node}).begin();
    unsigned chosen = FindReusableInputSlot(*inputs++, vector, def_pos, bank);
    if (chosen == count && !vector && operations[node].operand_count == 2) {
        const auto opcode = operations[node].opcode;
        if (opcode == OpCode::Add || opcode == OpCode::And || opcode == OpCode::Or ||
            opcode == OpCode::Xor) {
            chosen = FindReusableInputSlot(*inputs, vector, def_pos, bank);
            result.homes[node].reuse_rhs = chosen != count;
        } else if (opcode == OpCode::AndNot && options && options->bmi1) {
            chosen = FindReusableInputSlot(*inputs, vector, def_pos, bank);
        }
    }
    reused = chosen != count;
    return chosen;
}

unsigned Allocator::TryReuseArmFPVectorInput(u32 node,
                                             std::span<const u32> bank,
                                             bool& reused) const {
    const auto count = static_cast<unsigned>(bank.size());
    const auto opcode = operations[node].opcode;
    if (opcode != OpCode::ArmFPBinary && opcode != OpCode::ArmFPTernary) return count;
    const auto def_pos = result.homes[node].definition_position;
    auto inputs = program.Operands(NodeId{node}).begin();
    unsigned chosen = count;
    if (opcode == OpCode::ArmFPTernary && operations[node].operand_count >= 3) {
        const auto arg0 = *inputs++;
        const auto arg1 = *inputs++;
        const auto arg2 = *inputs;
        chosen = FindReusableInputSlot(arg2, true, def_pos, bank);
        if (chosen == count) chosen = FindReusableInputSlot(arg0, true, def_pos, bank);
        if (chosen == count) chosen = FindReusableInputSlot(arg1, true, def_pos, bank);
    } else if (opcode == OpCode::ArmFPBinary && operations[node].operand_count >= 2) {
        chosen = FindReusableInputSlot(*inputs++, true, def_pos, bank);
        if (chosen == count) chosen = FindReusableInputSlot(*inputs, true, def_pos, bank);
    }
    reused = chosen != count;
    return chosen;
}

bool Allocator::HasZeroSpillCost(u32 id) const {
    const auto& home = result.homes[id];
    if (home.use_count > 1) return false;
    if (operations[id].opcode == OpCode::LoadImm && operations[id].type != Type::V128 &&
        home.uniform_offset == UINT32_MAX && home.auxiliary_offset == UINT32_MAX)
        return true;
    return home.uniform_offset != UINT32_MAX;
}

unsigned Allocator::SelectEvictionVictim(u32 node, std::span<const u32> bank) const {
    const auto count = static_cast<unsigned>(bank.size());
    unsigned chosen = count;
    u32 victim = node;
    for (unsigned j = 0; j < count; ++j) {
        const u32 candidate = bank[j];
        const bool za = HasZeroSpillCost(candidate);
        const bool zb = HasZeroSpillCost(victim);
        const bool cheaper =
                za != zb ? za : result.homes[candidate].last_use > result.homes[victim].last_use;
        if (cheaper) {
            victim = candidate;
            chosen = j;
        }
    }
    return chosen;
}

bool Allocator::SpillNode(u32 id, std::span<u32> residents) {
    auto& home = result.homes[id];
    const unsigned bytes = operations[id].type == Type::V128 ? 16 : 8;
    if (operations[id].opcode == OpCode::LoadImm && operations[id].type != Type::V128 &&
        home.uniform_offset == UINT32_MAX && home.auxiliary_offset == UINT32_MAX) {
        home.rematerialize = true;
    } else if (home.uniform_offset == UINT32_MAX && !Reserve(bytes, bytes, id, home.stack_offset)) {
        return false;
    }
    home.spilled = true;
    for (auto& resident : residents)
        if (resident == id) resident = NodeId::invalid;
    return true;
}

void Allocator::AssignRegisters() {
    std::array<std::array<u32, kRegisterBankSize>, 2> banks;
    for (auto& bank : banks) bank.fill(NodeId::invalid);
    for (auto b : result.block_order) {
        const auto& body = blocks[b];
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            if (operations[i].type == Type::Void || result.homes[i].terminal_only) continue;
            const bool vector = operations[i].type == Type::V128;
            const unsigned count = vector ? kRegisterBankSize : scalar_register_count;
            auto bank = std::span<u32>{banks[vector]}.first(count);
            auto& home = result.homes[i];
            ExpireDeadResidents(bank, home.definition_position);
            if (home.rematerialize) continue;
            if (!vector && TryRematerializeStoreUniformImm(i, body)) continue;

            bool reused = false;
            unsigned chosen = TryReuseOperandRegister(i, vector, bank, reused);
            for (unsigned j = 0; !reused && j < count; ++j) {
                if (bank[j] == NodeId::invalid) {
                    chosen = j;
                    break;
                }
            }
            if (!reused && chosen == count && vector)
                chosen = TryReuseArmFPVectorInput(i, bank, reused);
            if (!reused && chosen == count) chosen = SelectEvictionVictim(i, bank);
            if (chosen == count) {
                if (!SpillNode(i, bank)) return;
                continue;
            }
            if (!reused && bank[chosen] != NodeId::invalid && !SpillNode(bank[chosen], bank))
                return;
            home.reg = static_cast<std::uint8_t>(vector ? 2 + chosen : scalar_registers[chosen]);
            bank[chosen] = i;
        }
    }
    if (options && options->local_flags &&
        (!Reserve(8, 8, 0, result.flags_slot) || !Reserve(1, 1, 0, result.condition_slot)))
        return;
    result.spill_bytes = (result.spill_bytes + 15) & ~15u;
    result.call_offset = result.spill_bytes;
    if (result.has_calls) result.spill_bytes += 336;
}

}  // namespace swift::runtime::backend::x64::detail

namespace swift::runtime::backend::x64 {

void AllocateBlock(const ir::Program& program,
                   Allocation& allocation,
                   unsigned pinned_slots,
                   const CompileOptions* options) {
    detail::Allocator(program, ir::Scope::Block, allocation, pinned_slots, options).Run();
}

void AllocateFunction(const ir::Program& program,
                      Allocation& allocation,
                      unsigned pinned_slots,
                      const CompileOptions* options) {
    detail::Allocator(program, ir::Scope::Function, allocation, pinned_slots, options).Run();
}

Allocation AllocateBlock(const ir::Program& program, unsigned pinned_slots) {
    Allocation result;
    AllocateBlock(program, result, pinned_slots);
    return result;
}

Allocation AllocateFunction(const ir::Program& program, unsigned pinned_slots) {
    Allocation result;
    AllocateFunction(program, result, pinned_slots);
    return result;
}

}  // namespace swift::runtime::backend::x64
