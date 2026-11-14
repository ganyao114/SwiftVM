#include "allocation_detail.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace swift::runtime::backend::x64::detail {

Type Allocator::ViewOf(CellView cell) const {
    return cell.TypeView() != Type::Void       ? cell.TypeView()
           : cell.Kind() == OperandKind::Value ? operations[cell.Payload()].type
                                               : Type::U64;
}

bool Allocator::Immediate32Cell(CellView cell, unsigned width, std::int32_t& immediate) const {
    if (cell.Kind() != OperandKind::Immediate || (width != 32 && width != 64)) return false;
    auto value = cell.Payload();
    const auto bits = TypeBits(ViewOf(cell));
    if (bits && bits < 64) value &= (std::uint64_t{1} << bits) - 1;
    immediate = static_cast<std::int32_t>(value);
    return width == 32 || value == std::uint64_t(std::int64_t(immediate));
}

bool Allocator::IsZeroScalarCell(CellView cell, unsigned width) const {
    if (width != 32 && width != 64) return false;
    if (cell.Kind() == OperandKind::Immediate) {
        std::uint64_t val = cell.Payload();
        const auto ibits = TypeBits(ViewOf(cell));
        if (ibits && ibits < 64) val &= (std::uint64_t{1} << ibits) - 1;
        return (width == 32 ? static_cast<u32>(val) : val) == 0;
    }
    if (cell.Kind() != OperandKind::Value) return false;
    const auto id = static_cast<u32>(cell.Payload());
    const auto& op = operations[id];
    const auto& home = result.homes[id];
    if (op.Metadata() != OperationMetadata{} || home.auxiliary_offset != UINT32_MAX ||
        op.type == Type::V128)
        return false;
    if (op.opcode == OpCode::Zero) return true;
    if (op.opcode != OpCode::LoadImm) return false;
    std::uint64_t imm = program.Operands(NodeId{id}).front().Payload();
    const auto nbits = TypeBits(op.type);
    if (nbits && nbits < 64) imm &= (std::uint64_t{1} << nbits) - 1;
    const auto vbits = TypeBits(ViewOf(cell));
    if (vbits && vbits < 64) imm &= (std::uint64_t{1} << vbits) - 1;
    return (width == 32 ? static_cast<u32>(imm) : imm) == 0;
}

bool Allocator::IsZeroIdentityCopyNode(u32 node) const {
    const auto& op = operations[node];
    const auto code = op.opcode;
    const unsigned width = TypeBits(op.type);
    if ((code != OpCode::Or && code != OpCode::Xor && code != OpCode::Add && code != OpCode::Sub) ||
        op.operand_count != 2 || (width != 32 && width != 64) || op.flag_outputs ||
        result.homes[node].auxiliary_offset != UINT32_MAX)
        return false;
    const auto ops = program.Operands(NodeId{node});
    auto it = ops.begin();
    if (it == ops.end()) return false;
    const auto a0 = *it++;
    if (it == ops.end()) return false;
    const auto a1 = *it++;
    if (it != ops.end()) return false;
    CellView other{};
    if (code != OpCode::Sub && IsZeroScalarCell(a0, width))
        other = a1;
    else if (IsZeroScalarCell(a1, width))
        other = a0;
    else
        return false;
    if (other.Kind() == OperandKind::Expression || TypeBits(ViewOf(other)) < 32) return false;
    if (other.Kind() == OperandKind::Value && TypeBits(operations[other.Payload()].type) < 8)
        return false;
    return true;
}

bool Allocator::IsImmediateUniformStoreNode(u32 node) const {
    const auto& op = operations[node];
    if (op.opcode != OpCode::StoreUniform || op.Metadata() != OperationMetadata{} ||
        pinned_slots != 0)
        return false;
    const auto ops = program.Operands(NodeId{node});
    auto it = ops.begin();
    if (it == ops.end()) return false;
    const auto field = *it++;
    if (it == ops.end() || field.Kind() != OperandKind::Uniform) return false;
    const auto width = TypeBits(field.TypeView());
    if (width < 8 || width > 64) return false;
    const auto off = field.Payload();
    if (off > INT32_MAX) return false;
    const auto bytes = (width + 7) / 8;
    if (options && options->flags_offset != UINT32_MAX &&
        off < std::uint64_t{options->flags_offset} + 8 &&
        std::uint64_t{options->flags_offset} < off + bytes)
        return false;
    return true;
}

bool Allocator::RecordUse(u32 definition, u32 at, u32 block) {
    if (definition >= operations.size() || operations[definition].type == Type::Void ||
        result.homes[definition].definition_position >= at)
        return false;
    auto& home = result.homes[definition];
    const auto end = operations[definition].block.value == block
                             ? at
                             : cfg.UseEnd(home.definition_position, at);
    home.last_use = std::max(home.last_use, end);
    const u32 weight = end > at ? 16u : 1u;
    home.use_count =
            static_cast<std::uint16_t>(std::min<u32>(UINT16_MAX, u32{home.use_count} + weight));
    return true;
}

bool Allocator::RecordAllUses() {
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (body.terminal.HasValue() &&
            !RecordUse(body.terminal.value.value, cfg.Start(b) + body.node_count)) {
            ResetResult(CompileError::InvalidProgram, {}, BlockId{b});
            return false;
        }
        if (body.terminal.HasValue() && operations[body.terminal.value.value].block.value != b)
            result.homes[body.terminal.value.value].terminal_only = false;
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
                result.homes[definition].terminal_only = false;
                if (saved_flags) result.homes[definition].auxiliary_offset = 0;
            }
        }
    }
    return true;
}

void Allocator::MarkZeroImmediateRematerialization() {
    bool any_zero_imm = false;
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            const auto& op = operations[i];
            auto& home = result.homes[i];
            if (op.opcode == OpCode::LoadImm && (op.type == Type::U32 || op.type == Type::U64) &&
                op.Metadata() == OperationMetadata{} && home.use_count > 0 && !home.terminal_only &&
                home.auxiliary_offset == UINT32_MAX) {
                std::uint64_t imm = program.Operands(NodeId{i}).front().Payload();
                if (op.type == Type::U32) imm &= UINT32_MAX;
                if (imm == 0) {
                    home.rematerialize = true;
                    home.spilled = true;
                    any_zero_imm = true;
                }
            }
        }
    }
    if (!any_zero_imm) return;
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (body.terminal.HasValue()) {
            auto& terminal_home = result.homes[body.terminal.value.value];
            if (terminal_home.rematerialize) {
                terminal_home.rematerialize = false;
                terminal_home.spilled = false;
            }
        }
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            if (IsZeroIdentityCopyNode(i) || IsImmediateUniformStoreNode(i)) continue;
            for (const auto input : program.Operands(NodeId{i})) {
                if (input.Kind() != OperandKind::Value) continue;
                auto& def_home = result.homes[input.Payload()];
                if (def_home.rematerialize) {
                    def_home.rematerialize = false;
                    def_home.spilled = false;
                }
            }
        }
    }
}

bool Allocator::IsPlainU64Value(CellView cell) const {
    return cell.Kind() == OperandKind::Value && operations[cell.Payload()].type == Type::U64 &&
           (cell.TypeView() == Type::Void || cell.TypeView() == Type::U64);
}

bool Allocator::ConstantDisp32(CellView cell, bool negate, std::int64_t& out) const {
    if (cell.Kind() != OperandKind::Immediate) return false;
    std::uint64_t val = cell.Payload();
    const auto ibits = TypeBits(cell.TypeView() != Type::Void ? cell.TypeView() : Type::U64);
    if (ibits && ibits < 64) val &= (std::uint64_t{1} << ibits) - 1;
    const std::uint64_t d64 = negate ? (std::uint64_t{0} - val) : val;
    const auto d32 = static_cast<std::int32_t>(d64);
    if (d64 != std::uint64_t(std::int64_t(d32))) return false;
    out = d32;
    return true;
}

bool Allocator::HasSimpleScalarLoadAddress(u32 node) const {
    const auto& op = operations[node];
    if (op.operand_count == 1) return IsPlainU64Value(*program.Operands(NodeId{node}).begin());
    if (op.operand_count != 3) return false;
    auto it = program.Operands(NodeId{node}).begin();
    const auto header = *it++, base = *it++, imm = *it;
    std::int64_t disp{};
    return header.Kind() == OperandKind::Expression && header.Count() == 2 &&
           (header.ExpressionOp() == OperandOp::Add || header.ExpressionOp() == OperandOp::Sub) &&
           IsPlainU64Value(base) &&
           ConstantDisp32(imm, header.ExpressionOp() == OperandOp::Sub, disp);
}

void Allocator::TryFuseScalarLoadToVector(const BlockRecord& body, u32 node) {
    const auto& op = operations[node];
    auto& load_home = result.homes[node];
    if ((op.opcode != OpCode::LoadMemory && op.opcode != OpCode::LoadMemoryTSO) ||
        (op.type != Type::U32 && op.type != Type::U64) ||
        op.Metadata() != OperationMetadata{} || load_home.terminal_only ||
        load_home.use_count != 1 || load_home.auxiliary_offset != UINT32_MAX ||
        !HasSimpleScalarLoadAddress(node))
        return;

    u32 next = node + 1;
    while (next < body.first_node + body.node_count && operations[next].opcode == OpCode::Nop &&
           operations[next].Metadata() == OperationMetadata{})
        ++next;
    if (next >= body.first_node + body.node_count ||
        load_home.last_use != result.homes[next].definition_position)
        return;

    const auto& consumer = operations[next];
    const auto arg = *program.Operands(NodeId{next}).begin();
    const auto view_bits = TypeBits(arg.TypeView() != Type::Void ? arg.TypeView() : op.type);
    const bool fuse_bitcast = consumer.opcode == OpCode::BitCast && consumer.type == Type::V128 &&
                              consumer.Metadata() == OperationMetadata{} &&
                              view_bits == TypeBits(op.type);
    const bool fuse_dup64 = options && options->avx && op.type == Type::U64 &&
                            consumer.opcode == OpCode::VecDup64 && consumer.type == Type::V128 &&
                            consumer.Metadata() == OperationMetadata{} && view_bits == 64;
    if (!fuse_bitcast && !fuse_dup64) return;

    load_home.rematerialize = true;
    load_home.spilled = true;
    for (const auto input : program.Operands(NodeId{node})) {
        if (input.Kind() != OperandKind::Value) continue;
        auto& def_home = result.homes[input.Payload()];
        def_home.last_use = std::max(def_home.last_use, result.homes[next].definition_position);
    }
}

bool Allocator::IsUxtwSourceValue(CellView cell) const {
    if (cell.Kind() != OperandKind::Value) return false;
    const auto& src_op = operations[cell.Payload()];
    const auto view = cell.TypeView() != Type::Void ? cell.TypeView() : src_op.type;
    return (src_op.type == Type::U32 || src_op.type == Type::U64) &&
           (view == Type::U32 || view == Type::U64);
}

bool Allocator::CanFuseAddressNode(u32 block_id, u32 node, u32 expected_pos) const {
    const auto& op = operations[node];
    const auto& home = result.homes[node];
    return op.type == Type::U64 && op.Metadata() == OperationMetadata{} &&
           op.block.value == block_id && !home.terminal_only && home.use_count == 1 &&
           home.last_use == expected_pos && home.auxiliary_offset == UINT32_MAX &&
           !home.rematerialize;
}

bool Allocator::MatchUxtwAddressNode(u32 block_id,
                                     u32 node,
                                     u32 expected_pos,
                                     CellView& src) const {
    if (!CanFuseAddressNode(block_id, node, expected_pos)) return false;
    const auto& op = operations[node];
    if ((op.opcode == OpCode::ZeroExtend32To64 || op.opcode == OpCode::ZeroExtend32) &&
        op.operand_count == 1) {
        const auto arg = *program.Operands(NodeId{node}).begin();
        if (!IsUxtwSourceValue(arg)) return false;
        src = arg;
        return true;
    }
    if (op.opcode == OpCode::And && op.operand_count == 2) {
        auto it = program.Operands(NodeId{node}).begin();
        const auto arg0 = *it++, arg1 = *it;
        if (!IsUxtwSourceValue(arg0) || arg1.Kind() != OperandKind::Immediate) return false;
        std::uint64_t mask = arg1.Payload();
        const auto mbits = TypeBits(arg1.TypeView() != Type::Void ? arg1.TypeView() : Type::U64);
        if (mbits && mbits < 64) mask &= (std::uint64_t{1} << mbits) - 1;
        if (mask != UINT32_MAX) return false;
        src = arg0;
        return true;
    }
    return false;
}

bool Allocator::AddAddressLeaf(FusedAddressState& state, CellView cell) const {
    if (!IsPlainU64Value(cell)) return false;
    if (!state.has_base) {
        state.has_base = true;
        state.base = cell;
        return true;
    }
    if (!state.has_index) {
        state.has_index = true;
        state.index = cell;
        state.index_uxtw = false;
        state.scale = 1;
        return true;
    }
    return false;
}

bool Allocator::MatchAddressTree(u32 block_id,
                                 FusedAddressState& state,
                                 CellView cell,
                                 u32 expected_pos,
                                 unsigned depth) const {
    if (!IsPlainU64Value(cell)) return false;
    const u32 nid = static_cast<u32>(cell.Payload());
    const u32 npos = result.homes[nid].definition_position;
    if (depth >= 4 || state.fused_count >= state.fused.size() ||
        !CanFuseAddressNode(block_id, nid, expected_pos))
        return AddAddressLeaf(state, cell);

    const auto& op = operations[nid];
    if ((op.opcode == OpCode::Add || op.opcode == OpCode::Sub) && op.operand_count == 2) {
        auto it = program.Operands(NodeId{nid}).begin();
        const auto lhs = *it++, rhs = *it;
        std::int64_t disp = 0;
        if (ConstantDisp32(rhs, op.opcode == OpCode::Sub, disp) && state.disp + disp >= INT32_MIN &&
            state.disp + disp <= INT32_MAX) {
            auto next = state;
            next.disp += disp;
            next.fused[next.fused_count++] = nid;
            if (MatchAddressTree(block_id, next, lhs, npos, depth + 1)) {
                state = next;
                return true;
            }
        } else if (op.opcode == OpCode::Add && IsPlainU64Value(lhs) && IsPlainU64Value(rhs)) {
            auto next = state;
            next.fused[next.fused_count++] = nid;
            if (MatchAddressTree(block_id, next, lhs, npos, depth + 1) &&
                MatchAddressTree(block_id, next, rhs, npos, depth + 1)) {
                state = next;
                return true;
            }
            next = state;
            next.fused[next.fused_count++] = nid;
            if (AddAddressLeaf(next, lhs) &&
                MatchAddressTree(block_id, next, rhs, npos, depth + 1)) {
                state = next;
                return true;
            }
            next = state;
            next.fused[next.fused_count++] = nid;
            if (MatchAddressTree(block_id, next, lhs, npos, depth + 1) &&
                AddAddressLeaf(next, rhs)) {
                state = next;
                return true;
            }
        }
    } else if (op.opcode == OpCode::Add && op.operand_count == 3 && !state.has_index) {
        auto it = program.Operands(NodeId{nid}).begin();
        const auto lhs = *it++, expr = *it++, idx = *it;
        if (IsPlainU64Value(lhs) && expr.Kind() == OperandKind::Expression && expr.Count() == 1 &&
            expr.ExpressionOp() == OperandOp::AddShift && expr.ShiftAmount() <= 3 &&
            IsPlainU64Value(idx)) {
            auto next = state;
            next.fused[next.fused_count++] = nid;
            next.has_index = true;
            next.index = idx;
            next.index_uxtw = false;
            next.scale = 1u << expr.ShiftAmount();
            if (MatchAddressTree(block_id, next, lhs, npos, depth + 1)) {
                state = next;
                return true;
            }
        }
    } else if (op.opcode == OpCode::LslImm && op.operand_count == 2 && !state.has_index) {
        auto it = program.Operands(NodeId{nid}).begin();
        const auto idx = *it++, sh = *it;
        if (IsPlainU64Value(idx) && sh.Kind() == OperandKind::Immediate &&
            (sh.Payload() & 63) <= 3) {
            auto next = state;
            next.fused[next.fused_count++] = nid;
            next.has_index = true;
            next.scale = 1u << (sh.Payload() & 63);
            CellView uxtw_src{};
            const u32 idx_id = static_cast<u32>(idx.Payload());
            if (next.fused_count < next.fused.size() &&
                MatchUxtwAddressNode(block_id, idx_id, npos, uxtw_src)) {
                next.fused[next.fused_count++] = idx_id;
                next.index = uxtw_src;
                next.index_uxtw = true;
            } else {
                next.index = idx;
                next.index_uxtw = false;
            }
            state = next;
            return true;
        }
    } else if (!state.has_index) {
        CellView uxtw_src{};
        if (MatchUxtwAddressNode(block_id, nid, expected_pos, uxtw_src)) {
            state.fused[state.fused_count++] = nid;
            state.has_index = true;
            state.index = uxtw_src;
            state.index_uxtw = true;
            state.scale = 1;
            return true;
        }
    }
    return AddAddressLeaf(state, cell);
}

void Allocator::TryFuseMemoryAddressing(u32 block_id, u32 node) {
    const auto& op = operations[node];
    if (op.opcode != OpCode::LoadMemory && op.opcode != OpCode::LoadMemoryTSO &&
        op.opcode != OpCode::StoreMemory && op.opcode != OpCode::StoreMemoryTSO)
        return;
    const auto first = *program.Operands(NodeId{node}).begin();
    if (!IsPlainU64Value(first)) return;

    const auto& mem_home = result.homes[node];
    const u32 target_pos =
            mem_home.rematerialize ? mem_home.last_use : mem_home.definition_position;
    const bool is_store = op.opcode == OpCode::StoreMemory || op.opcode == OpCode::StoreMemoryTSO;
    FusedAddressState state;
    if (!MatchAddressTree(block_id, state, first, target_pos, 0) || !state.has_base ||
        state.fused_count == 0 || (is_store && state.has_index))
        return;

    for (unsigned k = 0; k < state.fused_count; ++k) {
        auto& fused_home = result.homes[state.fused[k]];
        fused_home.rematerialize = true;
        fused_home.spilled = true;
    }
    auto& base_home = result.homes[state.base.Payload()];
    base_home.last_use = std::max(base_home.last_use, target_pos);
    if (state.has_index) {
        auto& index_home = result.homes[state.index.Payload()];
        index_home.last_use = std::max(index_home.last_use, target_pos);
    }
}

bool Allocator::ObserveUses() {
    if (!RecordAllUses()) return false;
    MarkZeroImmediateRematerialization();
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            TryFuseScalarLoadToVector(body, i);
            TryFuseMemoryAddressing(b, i);
        }
    }
    return true;
}

unsigned Allocator::ComputeNodeFlagWrites(const Operation& op, CellRange inputs) const {
    unsigned outputs = op.flag_outputs & 15;
    switch (op.opcode) {
        case OpCode::ArmFPCompare:
            outputs |= 15;
            break;
        case OpCode::SetCarry:
        case OpCode::InvertCarry:
            outputs |= 1;
            break;
        case OpCode::SetOverflow:
            outputs |= 2;
            break;
        case OpCode::ClearFlags:
            outputs |= inputs.front().Payload() & 15;
            break;
        case OpCode::RestoreFlags:
        case OpCode::SaveFlags:
        case OpCode::BranchOnlyFlags: {
            auto input = inputs.begin();
            ++input;
            outputs |= (*input).Payload() & 15;
            break;
        }
        default:
            break;
    }
    return outputs;
}

void Allocator::ComputeBlockFlagSummaries(const UniformPolicy& uniforms,
                                          std::span<FlagSummary> facts,
                                          std::span<u32> offsets) {
    const auto size = static_cast<u32>(blocks.size());
    for (u32 b = 0; b < size; ++b) {
        const auto& body = blocks[b];
        unsigned needed{}, killed{};
        for (u32 at = body.first_node + body.node_count; at-- > body.first_node;) {
            const auto& op = operations[at];
            auto& home = result.homes[at];
            const auto inputs = program.Operands(NodeId{at});
            const unsigned outputs = ComputeNodeFlagWrites(op, inputs);
            const unsigned reads =
                    FlagReads(op.opcode, EffectsFor(op.opcode, op.Metadata()), inputs, uniforms) &
                    15;
            home.flag_reads = static_cast<std::uint8_t>(reads);
            home.flag_writes = static_cast<std::uint8_t>(outputs);
            needed &= ~outputs;
            killed |= outputs;
            needed |= reads;
        }
        facts[b].gen = static_cast<unsigned char>(needed);
        facts[b].kill = static_cast<unsigned char>(killed);
        if (body.terminal.kind == TerminalKind::Jump) ++offsets[body.terminal.first.value + 1];
        if (body.terminal.kind == TerminalKind::Branch) {
            ++offsets[body.terminal.first.value + 1];
            ++offsets[body.terminal.second.value + 1];
        }
    }
}

void Allocator::PropagateFlagLiveness(std::span<FlagSummary> facts,
                                      std::span<const u32> offsets,
                                      std::span<const u32> predecessors,
                                      std::span<u32> queue) {
    const auto size = blocks.size();
    for (u32 b = 0; b < size; ++b) queue[b] = b;
    std::size_t head{}, tail{}, pending = size;
    while (pending) {
        const auto b = queue[head];
        if (++head == size) head = 0;
        --pending;
        facts[b].queued = false;
        const auto& terminal = blocks[b].terminal;
        unsigned output = 15;
        if (terminal.kind == TerminalKind::Jump) output = result.flags_input[terminal.first.value];
        if (terminal.kind == TerminalKind::Branch)
            output = result.flags_input[terminal.first.value] |
                     result.flags_input[terminal.second.value];
        const auto input = facts[b].gen | (output & ~facts[b].kill);
        if (input == result.flags_input[b]) continue;
        result.flags_input[b] = static_cast<std::uint8_t>(input);
        for (auto at = offsets[b]; at < offsets[b + 1]; ++at) {
            const auto previous = predecessors[at];
            if (facts[previous].queued) continue;
            facts[previous].queued = true;
            queue[tail] = previous;
            if (++tail == size) tail = 0;
            ++pending;
        }
    }
}

void Allocator::AssignBlockFlagNeeds(u32 block_id) {
    const auto& body = blocks[block_id];
    const auto& terminal = body.terminal;
    unsigned other = 15, term = 0;
    if (terminal.kind == TerminalKind::Jump) other = result.flags_input[terminal.first.value];
    if (terminal.kind == TerminalKind::Branch) {
        other = result.flags_input[terminal.first.value] |
                result.flags_input[terminal.second.value];
        const auto& op = operations[terminal.value.value];
        if (result.homes[terminal.value.value].terminal_only) {
            std::optional<Condition> condition;
            if (op.opcode == OpCode::CondSet || op.opcode == OpCode::LocalCondSet)
                condition = static_cast<Condition>(
                        program.Operands(terminal.value).front().Payload());
            else if (op.opcode == OpCode::TestFlags || op.opcode == OpCode::TestNotFlags)
                condition = FlagCondition(program.Operands(terminal.value).front().Payload(),
                                          op.opcode == OpCode::TestNotFlags);
            if (condition)
                result.terminal_conditions[block_id] = static_cast<std::uint8_t>(*condition);
        }
    }
    for (u32 at = body.first_node + body.node_count; at-- > body.first_node;) {
        auto& home = result.homes[at];
        other &= ~home.flag_writes;
        term &= ~home.flag_writes;
        if (terminal.kind == TerminalKind::Branch && terminal.value.value == at &&
            result.terminal_conditions[block_id] != 255)
            term |= home.flag_reads;
        else
            other |= home.flag_reads;
        home.flags_need = static_cast<std::uint8_t>(other | (term << 4));
    }
}

bool Allocator::ExpressionPreservesNativeFlags(CellRange::Iterator& at,
                                               CellRange::Iterator end) const {
    if (at == end) return false;
    const auto cell = *at++;
    if (cell.Kind() != OperandKind::Expression)
        return TypeBits(ViewOf(cell)) >= 32 &&
               (cell.Kind() != OperandKind::Value ||
                TypeBits(operations[cell.Payload()].type) >= 8);
    if (cell.ExpressionOp() != OperandOp::Add &&
        !(cell.ExpressionOp() == OperandOp::AddShift && cell.ShiftAmount() <= 3))
        return false;
    for (unsigned i = 0; i < cell.Count(); ++i)
        if (!ExpressionPreservesNativeFlags(at, end)) return false;
    return true;
}

bool Allocator::InstructionPreservesNativeFlags(u32 node) const {
    const auto& op = operations[node];
    const auto code = op.opcode;
    const auto& home = result.homes[node];
    if (home.rematerialize || home.terminal_only) return true;
    switch (code) {
        case OpCode::Nop:
        case OpCode::EntryPoint:
        case OpCode::InvertCarry:
        case OpCode::FaultLocation:
        case OpCode::GuestSafepoint:
        case OpCode::GuestDispatchSafepoint:
        case OpCode::CondSet:
        case OpCode::LocalCondSet:
            return true;
        case OpCode::LoadUniform:
            return TypeBits(op.type) >= 8;
        case OpCode::StoreUniform:
            return TypeBits(program.Operands(NodeId{node}).front().TypeView()) >= 32;
        case OpCode::LoadImm:
            return TypeBits(op.type) >= 32;
        case OpCode::LoadMemory:
        case OpCode::LoadMemoryTSO:
        case OpCode::StoreMemory:
        case OpCode::StoreMemoryTSO: {
            if ((code == OpCode::LoadMemory || code == OpCode::LoadMemoryTSO) &&
                TypeBits(op.type) < 8)
                return false;
            const auto ops = program.Operands(NodeId{node});
            auto it = ops.begin();
            return ExpressionPreservesNativeFlags(it, ops.end());
        }
        default:
            break;
    }

    const unsigned width = TypeBits(op.type);
    if (op.flag_outputs || home.auxiliary_offset != UINT32_MAX || (width != 32 && width != 64))
        return false;
    if (IsZeroIdentityCopyNode(node)) return true;

    const auto ops = program.Operands(NodeId{node});
    auto it = ops.begin();
    if (it == ops.end()) return false;
    const auto first = *it++;
    if (TypeBits(ViewOf(first)) < 32 ||
        (first.Kind() == OperandKind::Value && TypeBits(operations[first.Payload()].type) < 8))
        return false;
    if (code == OpCode::ZeroExtend32 || code == OpCode::ZeroExtend32To64 ||
        code == OpCode::ZeroExtend64)
        return true;
    if (code == OpCode::RorImm)
        return options && options->bmi2 && TypeBits(ViewOf(first)) >= width;
    if (code == OpCode::LslImm && it != ops.end())
        return ((*it).Payload() & (width - 1)) <= 3;

    std::int32_t imm{};
    if (code == OpCode::Add && it != ops.end()) {
        const auto second = *it;
        auto next_it = it;
        ++next_it;
        if (next_it == ops.end() && Immediate32Cell(second, width, imm)) return true;
        return ExpressionPreservesNativeFlags(it, ops.end());
    }
    if (code == OpCode::Sub && it != ops.end()) {
        const auto second = *it++;
        return it == ops.end() && Immediate32Cell(second, width, imm) && imm != INT32_MIN;
    }
    if (code == OpCode::And && width == 64 && it != ops.end()) {
        const auto second = *it++;
        if (it == ops.end() && second.Kind() == OperandKind::Immediate) {
            std::uint64_t raw = second.Payload();
            const auto ibits = TypeBits(ViewOf(second));
            if (ibits && ibits < 64) raw &= (std::uint64_t{1} << ibits) - 1;
            return raw == UINT32_MAX;
        }
    }
    return false;
}

bool Allocator::ComputeBlockNativeFlagOutput(u32 block_id) const {
    const auto& body = blocks[block_id];
    const bool bmi1 = options && options->bmi1;
    bool native = false;
    for (u32 at = body.first_node; at < body.first_node + body.node_count; ++at) {
        const auto& op = operations[at];
        if (op.flag_outputs == 15 && result.homes[at].auxiliary_offset == UINT32_MAX &&
            (op.type == Type::U32 || op.type == Type::U64) &&
            ProducesNativeFlagsOp(op.opcode, bmi1))
            native = true;
        else if (!InstructionPreservesNativeFlags(at))
            native = false;
    }
    return native;
}

void Allocator::AnalyzeFlags() {
    std::array<FlagSummary, 8> local_facts;
    std::array<u32, 9> local_offsets{};
    std::array<u32, 16> local_predecessors;
    std::array<u32, 8> local_queue;
    std::vector<FlagSummary> large_facts;
    std::vector<u32> large_offsets, large_predecessors, large_queue;
    const auto size = blocks.size();
    std::span<FlagSummary> facts = local_facts;
    std::span<u32> offsets = local_offsets, predecessors = local_predecessors, queue = local_queue;
    if (size > local_facts.size()) {
        large_facts.resize(size);
        large_offsets.resize(size + 1);
        large_predecessors.resize(size * 2);
        large_queue.resize(size);
        facts = large_facts;
        offsets = large_offsets;
        predecessors = large_predecessors;
        queue = large_queue;
    }

    result.flags_input.assign(size, 15);
    UniformPolicy uniforms;
    uniforms.flags = UniformRange{options->flags_offset, 8};
    uniforms.pc = UniformRange{options->pc_offset, 8};
    ComputeBlockFlagSummaries(uniforms, facts, offsets);

    for (u32 b = 0; b < size; ++b) {
        offsets[b + 1] += offsets[b];
        queue[b] = offsets[b];
    }
    for (u32 b = 0; b < size; ++b) {
        const auto& terminal = blocks[b].terminal;
        if (terminal.kind == TerminalKind::Jump) predecessors[queue[terminal.first.value]++] = b;
        if (terminal.kind == TerminalKind::Branch) {
            predecessors[queue[terminal.first.value]++] = b;
            predecessors[queue[terminal.second.value]++] = b;
        }
    }
    PropagateFlagLiveness(facts, offsets, predecessors, queue);

    result.terminal_conditions.assign(size, 255);
    result.native_entry.assign(size, false);
    std::vector<bool> native_output(size);
    std::vector<u32> rank(size);
    for (u32 i = 0; i < result.block_order.size(); ++i) rank[result.block_order[i]] = i;
    for (u32 b = 0; b < size; ++b) {
        AssignBlockFlagNeeds(b);
        native_output[b] = ComputeBlockNativeFlagOutput(b);
    }
    for (u32 b = 0; b < size; ++b) {
        bool backedge = false, all_native = true;
        for (auto at = offsets[b]; at < offsets[b + 1]; ++at) {
            const auto previous = predecessors[at];
            if (rank[previous] < rank[b]) continue;
            backedge = true;
            all_native &= native_output[previous];
        }
        result.native_entry[b] = backedge && all_native && result.flags_input[b];
    }
}

bool Allocator::IsOrdinaryUniformRange(UniformRange range) const {
    const bool pinned = pinned_slots && (options->pinned_uniform_offset == UINT32_MAX ||
                                         Overlaps(range,
                                                  {options->pinned_uniform_offset,
                                                   pinned_slots * 8}));
    bool ordinary = !pinned;
    for (auto offset : {options->flags_offset,
                        options->pc_offset,
                        options->mxcsr_offset,
                        options->arm_fpcr_offset,
                        options->arm_fpsr_offset})
        ordinary &= offset == UINT32_MAX || IsImmediateArmFPCR(offset) ||
                    !Overlaps(range, {offset, 8});
    return ordinary;
}

bool Allocator::IsAdjacentToUniformStore(u32 producer_id,
                                         u32 store_id,
                                         UniformRange store_range) const {
    const auto& producer = operations[producer_id];
    if (producer.opcode != OpCode::VecLoadConst &&
        HasAnyEffect(EffectsFor(producer.opcode, producer.Metadata()), Effect::Control))
        return false;
    for (u32 between = producer_id + 1; between < store_id; ++between) {
        const auto& bop = operations[between];
        if (bop.Metadata() != OperationMetadata{}) return false;
        if (bop.opcode == OpCode::Nop || bop.opcode == OpCode::VecLoadConst ||
            bop.opcode == OpCode::VecSharedZero)
            continue;
        if (bop.opcode == OpCode::BitCast) {
            const auto barg = *program.Operands(NodeId{between}).begin();
            const bool fused_load = barg.Kind() == OperandKind::Value &&
                                    result.homes[barg.Payload()].rematerialize &&
                                    (operations[barg.Payload()].opcode == OpCode::LoadMemory ||
                                     operations[barg.Payload()].opcode == OpCode::LoadMemoryTSO);
            if (!fused_load) continue;
        }
        if (bop.opcode == OpCode::StoreUniform) {
            const auto bfield = *program.Operands(NodeId{between}).begin();
            const auto bbytes = (TypeBits(bfield.TypeView()) + 7) / 8;
            const UniformRange brange{u32(bfield.Payload()), bbytes};
            if (bfield.Kind() == OperandKind::Uniform && bbytes && bfield.Payload() < 1024 &&
                brange.offset + bbytes <= 1024 && IsOrdinaryUniformRange(brange) &&
                !Overlaps(store_range, brange))
                continue;
        }
        return false;
    }
    return true;
}

void Allocator::TryBackStoreUniform(u32 at,
                                    CellRange inputs,
                                    u32 block_end,
                                    u32 barrier,
                                    const std::array<u32, 128>& next_write) {
    const auto& op = operations[at];
    if (op.opcode != OpCode::StoreUniform || op.Metadata() != OperationMetadata{}) return;
    auto input = inputs.begin();
    const auto field = *input++, value = *input;
    if (value.Kind() != OperandKind::Value || value.Payload() >= at || at - value.Payload() > 6)
        return;
    const u32 prod_id = static_cast<u32>(value.Payload());
    if (operations[prod_id].block != op.block || result.homes[prod_id].rematerialize) return;

    const auto& producer = operations[prod_id];
    auto& backing = result.homes[prod_id];
    const auto bits = TypeBits(producer.type);
    const auto bytes = TypeBits(field.TypeView()) / 8;
    const auto view = value.TypeView() == Type::Void ? producer.type : value.TypeView();
    const bool matching = producer.type == Type::V128
                                  ? view == Type::V128 && field.TypeView() == Type::V128
                                  : (producer.type == Type::U32 || producer.type == Type::U64) &&
                                            view != Type::V128 &&
                                            (field.TypeView() == Type::U32 ||
                                             field.TypeView() == Type::U64);
    const UniformRange range{u32(field.Payload()), bytes};
    if (!matching || !bytes || field.Payload() >= 1024 || range.offset + bytes > 1024 ||
        bits > bytes * 8 || TypeBits(view) < bits || backing.last_use > block_end ||
        !IsOrdinaryUniformRange(range) || !IsAdjacentToUniformStore(prod_id, at, range))
        return;

    auto limit = barrier;
    for (auto slot = range.offset / 8; slot <= (range.offset + bytes - 1) / 8; ++slot)
        limit = std::min(limit, next_write[slot]);
    if (backing.last_use < limit) {
        backing.uniform_offset = range.offset;
        backing.uniform_store_bytes = static_cast<std::uint8_t>(bytes);
    }
}

void Allocator::TryBackLoadUniform(u32 at,
                                   CellRange inputs,
                                   u32 block_end,
                                   u32 barrier,
                                   const std::array<u32, 128>& next_write) {
    const auto& op = operations[at];
    auto& home = result.homes[at];
    if (op.opcode != OpCode::LoadUniform || home.uniform_store_bytes || home.last_use > block_end ||
        (op.type != Type::U32 && op.type != Type::U64 && op.type != Type::V128))
        return;
    const auto field = inputs.front();
    const UniformRange range{u32(field.Payload()), TypeBits(op.type) / 8};
    if (field.Payload() >= 1024 || range.offset + range.bytes > 1024 ||
        !IsOrdinaryUniformRange(range))
        return;

    auto limit = barrier;
    for (auto slot = range.offset / 8; slot <= (range.offset + range.bytes - 1) / 8; ++slot)
        limit = std::min(limit, next_write[slot]);
    if (home.last_use < limit) home.uniform_offset = range.offset;
}

void Allocator::RecordUniformWriteHazards(u32 at,
                                          CellRange inputs,
                                          const UniformPolicy& policy,
                                          u32& barrier,
                                          std::array<u32, 128>& next_write) const {
    const auto& op = operations[at];
    const auto& home = result.homes[at];
    if (op.opcode == OpCode::VecLoadConst || op.opcode == OpCode::VecSharedZero) return;
    const auto effects = EffectsFor(op.opcode, op.Metadata());
    if (HasAnyEffect(effects, Effect::Unknown | Effect::External | Effect::Control) ||
        (!options->private_state && HasAnyEffect(effects, Effect::WriteMemory))) {
        barrier = home.definition_position;
        return;
    }
    const auto accesses = UniformEffectsFor(op, inputs, policy);
    if (accesses.all_write) barrier = home.definition_position;
    for (unsigned i = 0; i < accesses.count; ++i) {
        const auto& access = accesses.fields[i];
        if (!access.write || !access.range.bytes || access.range.offset >= 1024) continue;
        const auto last = std::min<std::uint64_t>(
                1024, std::uint64_t{access.range.offset} + access.range.bytes);
        u32 write_pos = home.definition_position;
        if (op.opcode == OpCode::StoreUniform && op.Metadata() == OperationMetadata{}) {
            auto sit = inputs.begin();
            ++sit;
            if (sit != inputs.end() && (*sit).Kind() == OperandKind::Value) {
                const auto& bh = result.homes[(*sit).Payload()];
                if (bh.uniform_store_bytes && bh.uniform_offset == access.range.offset)
                    write_pos = std::min(write_pos, bh.definition_position);
            }
        }
        for (auto slot = access.range.offset / 8; slot <= (last - 1) / 8; ++slot)
            next_write[slot] = std::min(next_write[slot], write_pos);
    }
}

void Allocator::AnalyzeUniformBacking() {
    if (!options) return;
    UniformPolicy policy;
    policy.flags = UniformRange{options->flags_offset, 8};
    policy.pc = UniformRange{options->pc_offset, 8};
    policy.numeric_control = UniformRange{options->mxcsr_offset, 4};
    for (const auto b : result.block_order) {
        const auto& body = blocks[b];
        if (!body.node_count) continue;
        std::array<u32, 128> next_write;
        next_write.fill(UINT32_MAX);
        u32 barrier = UINT32_MAX;
        const auto block_end =
                result.homes[body.first_node].definition_position + body.node_count;
        for (u32 at = body.first_node + body.node_count; at-- > body.first_node;) {
            const auto inputs = program.Operands(NodeId{at});
            TryBackStoreUniform(at, inputs, block_end, barrier, next_write);
            TryBackLoadUniform(at, inputs, block_end, barrier, next_write);
            RecordUniformWriteHazards(at, inputs, policy, barrier, next_write);
        }
    }
}

}  // namespace swift::runtime::backend::x64::detail
