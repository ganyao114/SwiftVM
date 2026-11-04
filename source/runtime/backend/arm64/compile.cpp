#include "emit.h"

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned bits, bool sign) noexcept;
std::uintptr_t CpuidHelper() noexcept;
namespace detail {

bool Writer::Run() {
    if (NativeNZCV()) RequireFlags();
    if (Enabled() && options.flags_offset > UINT32_MAX - 7) Reject(CompileError::UnsupportedFlags);
    if (Linked() && (!options.dispatcher || !options.exit_handler || options.pc_offset > UINT32_MAX - 7))
        Reject(CompileError::UnsupportedOperand);
    if (options.dispatch_mode != DispatchMode::Callback && !Linked()) Reject(CompileError::UnsupportedOperand);
    if (Linked() && Enabled() && options.flags_offset < u64{options.pc_offset} + 8 &&
        options.pc_offset < u64{options.flags_offset} + 8) Reject(CompileError::UnsupportedOperand);
    if (options.private_uniforms && (!Linked() || !Pinned())) Reject(CompileError::UnsupportedOperand);
    if (VectorPinned() && (!Linked() || !options.private_uniforms || !options.fault_locations ||
            options.pinned_vector_offset > UINT32_MAX - 255u)) Reject(CompileError::UnsupportedOperand);
    if (VectorPinned()) {
        const auto overlaps = [&](u32 field, unsigned bytes) {
            return field != UINT32_MAX && u64{field} < u64{options.pinned_vector_offset} + 256 &&
                    options.pinned_vector_offset < u64{field} + bytes;
        };
        if (overlaps(options.pc_offset, 8) || overlaps(options.flags_offset, 8) ||
                overlaps(options.pinned_uniform_offset, options.pinned_uniform_slots * 8u))
            Reject(CompileError::UnsupportedOperand);
    }
    if (options.deferred_pc && (!Linked() || !options.private_uniforms || !options.fault_locations))
        Reject(CompileError::UnsupportedOperand);
    if (Pinned()) {
        if (!Linked() || (options.pinned_uniform_slots != 8 && options.pinned_uniform_slots != 12 &&
                options.pinned_uniform_slots != 16))
            Reject(CompileError::UnsupportedOperand);
        const auto bytes = options.pinned_uniform_slots * 8u;
        if (options.pinned_uniform_offset > UINT32_MAX - (bytes - 1)) Reject(CompileError::UnsupportedOperand);
        const auto overlaps = [&](u32 field) {
            return field != UINT32_MAX && u64{field} < u64{options.pinned_uniform_offset} + bytes &&
                    options.pinned_uniform_offset < u64{field} + 8;
        };
        if (overlaps(options.pc_offset) || overlaps(options.flags_offset)) Reject(CompileError::UnsupportedOperand);
    }
    if (!Linked()) {
        Put(0xa9bf7bf3); // STP x19,x30,[sp,#-16]!
        Move(19, 0);
    }
    StackAdjust(true);
    if (!Linked()) Shadow(true);
    if (allocation.publish_entry_pc) { Immediate(0, options.entry_pc); PCMemory(false); }
    // Consume allocation's one CFG schedule; source IDs/storage are not
    // rewritten. Entry's reachable component is first, then unreachable.
    for (std::size_t order = 0; order < allocation.block_order.size(); ++order) {
        const auto b = allocation.block_order[order];
        block = BlockId{b}; labels[b] = static_cast<u32>(output.words.size());
        // Any predecessor may arrive with a newer flags carrier. Within
        // this body, one bit tracks whether its state slot needs a store.
        shadow_dirty = Enabled();
        nzcv_matches = 0;
        packed_current = false;
        terminal_condition = 18;
        has_fault_location = false;
        static_pc_valid = false;
        alternate_pc_valid = false;
        dispatch_emitted = false;
        elided_consumer = {};
        deferred_parity = 0; direct_carry = false;
        const auto& body = source.Blocks()[b];
        deferred_test = {};
        deferred_test.active = !deferred_test_used &&
                RawAF() && Pinned() && options.private_uniforms && options.fault_locations && options.recover_unaligned &&
                ((source.ScopeKind() == Scope::Function && body.node_count >= 3 &&
                  body.terminal.kind == TerminalKind::Branch && body.terminal.first == block &&
                  body.terminal.second != block &&
                  body.terminal.value.value == body.first_node + body.node_count - 1) ||
                 (source.ScopeKind() == Scope::Block && !output.frame_bytes && !allocation.publish_entry_pc &&
                  body.node_count >= 4 && body.terminal.kind == TerminalKind::Dispatch));
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            node = NodeId{i};
            const auto& operation = source.Operations()[i];
            if ((operation.opcode == OpCode::Nop || operation.opcode == OpCode::FaultLocation) &&
                    operation.Metadata() == OperationMetadata{}) {
                if (operation.opcode == OpCode::Nop) continue;
                if (!options.fault_locations) Reject(CompileError::UnsupportedOpcode);
                fault_location = IntegerImmediate(source.Operands(node).front());
                has_fault_location = true;
                continue;
            }
            const auto first = static_cast<u32>(output.words.size());
            unaligned_word = UINT32_MAX;
            fault_parity = fault_carry = 0;
            const auto effects = EffectsFor(operation.opcode, operation.Metadata());
            Emit(operation, effects);
            if (options.fault_locations && has_fault_location &&
                HasAnyEffect(effects, Effect::MayFault)) {
                output.faults.push_back({first, static_cast<u32>(output.words.size()), fault_location, unaligned_word});
                output.faults.back().parity_register = fault_parity;
                output.faults.back().carry_direct = fault_carry;
                if (deferred_test.active)
                    deferred_test.sites[deferred_test.site_count++] = output.faults.size() - 1;
            }
        }
        if (pending_save) Reject(CompileError::UnsupportedFlags);
        const auto next = order + 1 < allocation.block_order.size() ? BlockId{allocation.block_order[order + 1]} : BlockId{};
        node = {}; if (!dispatch_emitted) Terminal(body.terminal, next);
    }
    for (const auto& fixup : fixups) {
        const auto distance = static_cast<std::int64_t>(labels[fixup.target]) - fixup.word;
        auto& instruction = output.words[fixup.word];
        if ((instruction & 0x7c000000u) == 0x14000000u) {
            if (distance < -(std::int64_t{1} << 25) || distance >= (std::int64_t{1} << 25))
                Reject(CompileError::CodeTooLarge);
            instruction = (instruction & ~0x03ffffffu) | (static_cast<u32>(distance) & 0x03ffffffu);
        } else {
            if (distance < -(std::int64_t{1} << 18) || distance >= (std::int64_t{1} << 18)) {
                if (!long_forward_branches) return false;
                Reject(CompileError::CodeTooLarge);
            }
            instruction = (instruction & ~(0x7ffffu << 5)) | ((static_cast<u32>(distance) & 0x7ffffu) << 5);
        }
    }
    // Close only the actual recoverable sites, after every hot block and link
    // stub. A SIGBUS patches the one fast instruction to this cold sequence.
    for (auto& site : output.faults) {
        if (site.unaligned_word == UINT32_MAX) continue;
        const auto fallback = output.words.size();
        const auto outbound = static_cast<std::int64_t>(fallback) - site.unaligned_word;
        const auto inbound = static_cast<std::int64_t>(site.unaligned_word) + 1 -
                (static_cast<std::int64_t>(fallback) + 2);
        if (outbound < -(std::int64_t{1} << 25) || outbound >= (std::int64_t{1} << 25) ||
                inbound < -(std::int64_t{1} << 25) || inbound >= (std::int64_t{1} << 25))
            Reject(CompileError::CodeTooLarge);
        const auto fast = output.words[site.unaligned_word];
        const auto opcode = fast & 0x3ffffc00u;
        const auto unscaled = fast & 0x3fe00c00u;
        const bool imm9 = unscaled == 0x19400000u || unscaled == 0x19000000u ||
                unscaled == 0x19c00000u || unscaled == 0x19800000u;
        const bool load = opcode == 0x08dffc00u || opcode == 0x38bfc000u ||
                unscaled == 0x19400000u || unscaled == 0x19c00000u || unscaled == 0x19800000u;
        const auto plain = imm9 ? fast ^ 0x21000000u :
                (load ? 0x39400000u : 0x39000000u) | (fast & 0xc00003ffu);
        site.unaligned_fallback_word = static_cast<u32>(fallback);
        if (load) { Put(plain); Put(0xd50339bf); }
        else { Put(0xd5033bbf); Put(plain); }
        Put(0x14000000u | (static_cast<u32>(inbound) & 0x03ffffffu));
    }
    return true;
}

void Writer::StackAdjust(bool subtract) {
    const auto spill_frame = output.frame_bytes - (Linked() ? 0 : 16);
    const u32 high = spill_frame >> 12, low = spill_frame & 4095;
    const auto instruction = subtract ? 0xd10003ffu : 0x910003ffu;
    if (high) Put(instruction | (1u << 22) | (high << 10));
    if (low) Put(instruction | (low << 10));
}

void Writer::PCMemory(bool load) {
    if (!RawAF()) { Memory(load, 64, 0, 29); return; }
    const auto offset = options.pc_offset;
    if (!(offset & 7) && offset / 8 <= 4095) Memory(load, 64, 0, 19, offset / 8);
    else {
        Immediate(17, offset); Put(0x8b000000u | (17u << 16) | (19u << 5) | 17u);
        Memory(load, 64, 0, 17);
    }
}

void Writer::OrderedMemory(bool load, unsigned bits, unsigned value, unsigned address, std::int32_t offset) {
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) Reject(CompileError::UnsupportedType);
    const auto size = bits == 64 ? 3u : bits == 32 ? 2u : bits == 16 ? 1u : 0u;
    // Without an installed service and real PC marker, retain the actual
    // alignment guard. Recoverable accesses keep only the actual fast
    // instruction here; Run closes their cold half-barrier fallbacks.
    const bool recover = size && options.fault_locations && options.recover_unaligned && has_fault_location;
    if (size && !recover) { Extract(0, address, 0, size); Put(0xb5000060u); }
    const bool imm9 = RcpcImm9();
    const auto opcode = imm9 ? (load ? 0x19400000u : 0x19000000u) :
            load ? (options.rcpc_loads ? 0x38bfc000u : 0x08dffc00u) : 0x089ffc00u;
    const auto instruction = opcode | (size << 30) | (address << 5) | value |
            (imm9 ? (static_cast<u32>(offset) & 511u) << 12 : 0u);
    if (recover) {
        unaligned_word = static_cast<u32>(output.words.size());
        Put(instruction);
        return;
    }
    Put(instruction);
    if (!size) return;
    Put(0x14000003u);
    if (load) { Memory(true, bits, value, address); Put(0xd50339bf); }
    else { Put(0xd5033bbf); Memory(false, bits, value, address); }
}

bool Writer::PartialByteStore(const Operation& operation, const Home& home) const {
    if (operation.type != Type::U8 || home.spilled || home.reg == 1 ||
            node.value + 1 >= source.Operations().size()) return false;
    const NodeId consumer{node.value + 1};
    const auto& next = source.Operations()[consumer.value];
    if (next.block != operation.block || next.opcode != OpCode::StoreUniform ||
            next.Metadata() != OperationMetadata{} ||
            home.last_use != allocation.homes[consumer.value].definition_position) return false;
    const auto fields = source.Operands(consumer); auto at = fields.begin();
    if (at == fields.end()) return false;
    const auto field = *at++;
    if (at == fields.end()) return false;
    const auto value = *at++;
    return at == fields.end() && field.Kind() == OperandKind::Uniform && field.TypeView() == Type::U8 &&
            field.Payload() <= UINT32_MAX && value.Kind() == OperandKind::Value && value.Payload() == node.value &&
            View(value) == Type::U8 && PinnedSlot(static_cast<u32>(field.Payload()), 1) >= 0;
}

unsigned Writer::Expression(Cursor& cursor, unsigned first, unsigned second, unsigned native_width) {
    const auto input = cursor.Take();
    if (input.Kind() != OperandKind::Expression) return Read(input, first, 64, false, native_width);
    if (input.Count() != 2) Reject(CompileError::UnsupportedOperand);
    const auto expression = input.ExpressionOp();
    if ((expression != OperandOp::Add && expression != OperandOp::Sub && expression != OperandOp::AddShift) ||
        (input.ShiftAmount() && expression != OperandOp::AddShift) || input.ShiftAmount() >= 64)
        Reject(CompileError::UnsupportedOperand);
    // A W consumer needs only the expression's low 32 bits. Large shifts
    // retain the X expression and let that consumer take its ordinary view.
    if (input.ShiftAmount() >= 32) native_width = 64;
    const auto wide = native_width == 64 ? 1u << 31 : 0u;
    const auto base = cursor.Take(), operand = cursor.Take();
    if ((expression == OperandOp::Add || expression == OperandOp::Sub) && operand.Kind() == OperandKind::Immediate) {
        auto literal = IntegerImmediate(operand);
        const auto bits = Width(operand.TypeView());
        if (bits < 64) literal &= (u64{1} << bits) - 1;
        if (native_width == 32) literal &= UINT32_MAX;
        bool subtract = expression == OperandOp::Sub;
        if ((native_width == 32 && (literal >> 31)) || (bits == 64 && (literal >> 63))) {
            literal = u64{} - literal;
            if (native_width == 32) literal &= UINT32_MAX;
            subtract = !subtract;
        }
        const bool shifted = literal > 4095 && !(literal & 4095);
        const auto field = shifted ? literal >> 12 : literal;
        if (field <= 4095) {
            const auto left = Read(base, first, 64, false, native_width);
            Put((subtract ? 0x51000000u : 0x11000000u) | wide | (static_cast<u32>(shifted) << 22) |
                    (static_cast<u32>(field) << 10) | (left << 5) | first);
            return first;
        }
    }
    if (native_width == 64 && operand.Kind() == OperandKind::Value && input.ShiftAmount() <= 4) {
        const auto bits = std::min(Width(View(operand)), allocation.homes[operand.Payload()].CarrierBits());
        if (bits == 32) {
            const auto left = Read(base, first), right = RawValue(static_cast<u32>(operand.Payload()), second, true);
            // The actual W index performs the logical truncation. Do not
            // materialize a zero-extended X temporary before address ADD.
            Put((expression == OperandOp::Sub ? 0xcb204000u : 0x8b204000u) |
                    (right << 16) | (static_cast<u32>(input.ShiftAmount()) << 10) | (left << 5) | first);
            return first;
        }
    }
    const auto left = Read(base, first, 64, false, native_width),
            right = Read(operand, second, 64, false, native_width);
    Put((expression == OperandOp::Sub ? 0x4b000000u : 0x0b000000u) | wide |
            (right << 16) | (static_cast<u32>(input.ShiftAmount()) << 10) | (left << 5) | first);
    return first;
}

unsigned Writer::OrderedAddress(Cursor& cursor, std::int32_t& offset, bool store) {
    if (!store && !RcpcImm9()) return Expression(cursor, 16, 17);
    auto probe = cursor;
    const auto expression = probe.Take();
    if (expression.Kind() != OperandKind::Expression || expression.Count() != 2 || expression.ShiftAmount() ||
            (expression.ExpressionOp() != OperandOp::Add && expression.ExpressionOp() != OperandOp::Sub))
        return Expression(cursor, 16, 17);
    const auto base = probe.Take(), literal = probe.Take();
    if (base.Kind() != OperandKind::Value || literal.Kind() != OperandKind::Immediate || literal.TypeView() != Type::U64)
        return Expression(cursor, 16, 17);
    auto displacement = literal.Payload();
    if (expression.ExpressionOp() == OperandOp::Sub) displacement = u64{} - displacement;
    const auto end = probe.at;
    if (store) {
        const auto value = probe.Take();
        if (value.Kind() == OperandKind::Value && source.Operations()[value.Payload()].type == Type::V128) {
            // STR Q uses an unsigned imm12 scaled by 16, independently of RCpc.
            if (displacement > 4095u * 16u || (displacement & 15)) return Expression(cursor, 16, 17);
            offset = static_cast<std::int32_t>(displacement); // Keep the shared output in bytes.
            cursor.at = end;
            return Read(base, 16);
        }
    }
    if (!RcpcImm9() || (displacement > 255 && displacement < u64{} - 256)) return Expression(cursor, 16, 17);
    offset = displacement <= 255 ? static_cast<std::int32_t>(displacement) :
            -static_cast<std::int32_t>(u64{} - displacement);
    cursor.at = end;
    return Read(base, 16);
}

bool Writer::ConstantPC(CellView value, u64& pc) const {
    if (value.Kind() != OperandKind::Value || Width(View(value)) != 64) return false;
    const auto& producer = source.Operations()[value.Payload()];
    if (producer.opcode != OpCode::LoadImm || producer.block != block || Bits(producer.type) != 64) return false;
    pc = source.Operands(NodeId{static_cast<u32>(value.Payload())}).front().Payload();
    return true;
}

void Writer::ObservePCStore(CellView uniform, CellView value) {
    if (!Linked()) return;
    const auto size = TypeBits(uniform.TypeView()) / 8;
    if (uniform.Payload() >= u64{options.pc_offset} + 8 || options.pc_offset >= uniform.Payload() + size) return;
    static_pc_valid = false;
    alternate_pc_valid = false;
    if (uniform.Payload() != options.pc_offset || size != 8 || value.Kind() != OperandKind::Value ||
        Width(View(value)) != 64) return;
    if (ConstantPC(value, static_pc)) { static_pc_valid = true; return; }
    const auto& producer = source.Operations()[value.Payload()];
    if (producer.opcode == OpCode::Select && producer.block == block && Bits(producer.type) == 64) {
        auto at = source.Operands(NodeId{static_cast<u32>(value.Payload())}).begin(); ++at;
        const auto yes = *at++, no = *at;
        static_pc_valid = ConstantPC(yes, static_pc) && ConstantPC(no, alternate_pc);
        alternate_pc_valid = static_pc_valid && static_pc != alternate_pc;
    }
}

void Writer::Emit(const Operation& operation, Effect effects) {
    const auto metadata = operation.Metadata();
    const bool test_prefix = deferred_test.active && (metadata == OperationMetadata{} || operation.opcode == OpCode::And);
    // Only the actual supported prefix cases below keep this candidate
    // alive. All other operations follow normal emission unchanged.
    deferred_test.active = false;
    if (static_pc_valid && HasAnyEffect(effects,
            Effect::WriteMemory | Effect::Unknown | Effect::External)) static_pc_valid = false;
    if (static_pc_valid && operation.opcode != OpCode::StoreUniform &&
        HasAnyEffect(effects, Effect::WriteGuestState)) static_pc_valid = false;
    Metadata(operation);
    const auto legacy_mask = CaptureLegacy(operation);
    const auto flag_mask = metadata.flag_outputs | legacy_mask;
    if (flag_mask || HasAnyEffect(effects, Effect::WriteFlags)) shadow_dirty = true;
    const auto cells = source.Operands(node); Cursor cursor{cells.begin(), cells.end(), *this};
    const bool produces = operation.type != Type::Void;
    const auto& home = allocation.homes[node.value];
    const bool vector = operation.type == Type::V128;
    const unsigned destination = produces && !home.spilled ? home.reg : vector ? VectorTemporary(true) : 16;
    const auto width = produces && !vector ? Width(operation.type) : 0;
    // Full literal writers can replace the old local representation. All
    // actual readers and opaque boundaries retain the ordinary complete ABI.
    const bool reads_flags = HasAnyEffect(effects, Effect::ReadFlags);
    const auto pf_af = parity | auxiliary;
    const bool replaces_pf = !reads_flags && (metadata.flag_outputs & pf_af) == pf_af;
    if (replaces_pf) deferred_parity = 0;
    if (!reads_flags && (metadata.flag_outputs & carry)) direct_carry = false;
    const bool memory = operation.opcode == OpCode::LoadMemory || operation.opcode == OpCode::LoadMemoryTSO ||
            operation.opcode == OpCode::StoreMemory || operation.opcode == OpCode::StoreMemoryTSO;
    if (reads_flags || legacy_mask || HasAnyEffect(effects, Effect::Unknown | Effect::External | Effect::Control) ||
        (metadata == OperationMetadata{} && HasAnyEffect(effects, Effect::WriteFlags)) ||
        (HasAnyEffect(effects, Effect::MayFault) && (!has_fault_location || !memory || vector)) ||
        operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid) CompleteLocalFlags();
    else if ((metadata.flag_outputs & pf_af) && !replaces_pf) CompleteParity();
    // Borrowed/self Uniform loads are handled at their actual read/write
    // encoding point, so a no-op state read does not force a PF copy.
    if (deferred_parity && produces && !vector && operation.opcode != OpCode::LoadUniform && destination == deferred_parity)
        CompleteParity();
    bool saved{};
    const auto save_result = [&] {
        if (produces && home.spilled) {
            if (vector) VectorMemory(false, destination, 31, home.stack_offset / 16);
            else Memory(false, 64, destination, 31, home.stack_offset / 8);
            saved = true;
        }
    };
    if ((vector || operation.opcode == OpCode::VecExtract64 || operation.opcode == OpCode::VecExtract16 ||
            operation.opcode == OpCode::VecMovMask || operation.opcode == OpCode::VecFCmp) &&
        EmitVector(operation, cursor, destination)) { cursor.Done(); save_result(); return; }
    switch (operation.opcode) {
        case OpCode::Zero: Immediate(destination, 0); break;
        case OpCode::LoadImm: {
            const auto input = cursor.Take();
            if (input.Kind() != OperandKind::Immediate) Reject(CompileError::UnsupportedOperand);
            Width(input.TypeView());
            if (!home.terminal_only)
                Immediate(destination, width == 64 ? input.Payload() : input.Payload() & ((u64{1} << width) - 1));
            break;
        }
        case OpCode::LoadUniform: case OpCode::StoreUniform: case OpCode::StoreUniformSelectImm:
        case OpCode::GetFlags: case OpCode::RestoreFlags: case OpCode::SaveFlags:
        case OpCode::ClearFlags: case OpCode::PublishFCmpFlags: case OpCode::TestFlags:
        case OpCode::TestNotFlags: case OpCode::SetCarry: case OpCode::SetOverflow:
        case OpCode::InvertCarry: case OpCode::LocalParitySet:
            EmitState(operation, cursor, destination, home, test_prefix); break;
        case OpCode::LoadMemory: case OpCode::LoadMemoryTSO: {
            if (test_prefix) {
                const auto address = *cursor.at;
                deferred_test.active = deferred_test.captured && !deferred_test.loaded && has_fault_location &&
                        !deferred_test.site_count && (operation.type == Type::U32 || operation.type == Type::U64) &&
                        address.Kind() == OperandKind::Value && address.Payload() == deferred_test.snapshot.value &&
                        (address.TypeView() == Type::Void || address.TypeView() == Type::U64);
                if (deferred_test.active) deferred_test.loaded = node;
            }
            if (!options.private_uniforms) Shadow(false); // Private ABI recovers x1 from the actual fault context.
            const bool ordered = operation.opcode == OpCode::LoadMemoryTSO;
            std::int32_t offset{};
            const auto address = ordered ? OrderedAddress(cursor, offset) : Expression(cursor, 16, 17);
            fault_parity = static_cast<std::uint8_t>(deferred_parity);
            fault_carry = direct_carry;
            if (ordered && (operation.type == Type::U16 || operation.type == Type::U32) && metadata == OperationMetadata{} &&
                    RcpcImm9() && !home.spilled && node.value + 1 < source.Operations().size()) {
                const NodeId consumer{node.value + 1};
                const auto& next = source.Operations()[consumer.value];
                const auto& next_home = allocation.homes[consumer.value];
                if (next.opcode == OpCode::SignExtend && next.type == (width == 16 ? Type::U32 : Type::U64) &&
                        next.block == operation.block && next.Metadata() == OperationMetadata{} &&
                        !next_home.spilled && home.last_use == next_home.definition_position &&
                        next_home.reg != deferred_parity) {
                    const auto inputs = source.Operands(consumer); auto at = inputs.begin();
                    if (at != inputs.end()) {
                        const auto input = *at++;
                        if (at == inputs.end() && input.Kind() == OperandKind::Value &&
                                input.Payload() == node.value && Width(View(input)) == width) {
                            // The adjacent pure SXT is the load's only use. A fault
                            // leaves its destination (including a guest pin) unchanged.
                            unaligned_word = static_cast<u32>(output.words.size());
                            Put((width == 16 ? 0x59c00000u : 0x99800000u) |
                                    ((static_cast<u32>(offset) & 511u) << 12) |
                                    (address << 5) | next_home.reg); // LDAPURSH W / LDAPURSW X.
                            elided_consumer = consumer;
                            saved = true;
                            break;
                        }
                    }
                }
            }
            if (ordered) OrderedMemory(true, width, destination, address, offset);
            else Memory(true, width, destination, address);
            break;
        }
        case OpCode::StoreMemory: case OpCode::StoreMemoryTSO: {
            if (test_prefix) {
                const auto address = *cursor.at;
                deferred_test.active = deferred_test.value && has_fault_location && deferred_test.site_count == 1 &&
                        address.Kind() == OperandKind::Value && address.Payload() == deferred_test.snapshot.value &&
                        (address.TypeView() == Type::Void || address.TypeView() == Type::U64);
            }
            if (!options.private_uniforms) Shadow(false);
            const bool ordered = operation.opcode == OpCode::StoreMemoryTSO;
            std::int32_t offset{};
            const auto address = ordered ? OrderedAddress(cursor, offset, true) : Expression(cursor, 16, 17);
            const auto input = cursor.Take();
            if (deferred_test.active) {
                const auto type = input.Kind() == OperandKind::Value ? source.Operations()[input.Payload()].type : Type::Void;
                deferred_test.active = IntegerType(type) && TypeBits(type) <= 64;
            }
            const bool vector_input = input.Kind() == OperandKind::Value && source.Operations()[input.Payload()].type == Type::V128;
            const auto bits = vector_input ? 128u : Width(View(input));
            const auto value = vector_input ? VectorValue(input, 0) : Read(input, 17, 64, false, bits == 32 ? 32 : 64);
            if (vector_input) {
                CompleteLocalFlags();
                if (operation.opcode == OpCode::StoreMemoryTSO) Put(0xd5033bbf);
                VectorMemory(false, value, address, static_cast<u32>(offset) / 16);
            } else {
                fault_parity = static_cast<std::uint8_t>(deferred_parity);
                fault_carry = direct_carry;
                if (ordered) OrderedMemory(false, bits, value, address, offset);
                else Memory(false, bits, value, address);
            }
            if (!options.private_uniforms) Shadow(true); // Generic raw pointers may alias the flags field.
            break;
        }
        case OpCode::CompareAndSwap: case OpCode::AtomicExchange:
        case OpCode::AtomicFetchAdd: case OpCode::AtomicRMW: {
            if (width != 8 && width != 16 && width != 32 && width != 64)
                Reject(CompileError::UnsupportedType);
            const bool compare = operation.opcode == OpCode::CompareAndSwap;
            const bool exchange = operation.opcode == OpCode::AtomicExchange;
            auto kind = AtomicRMWOp::Add;
            if (operation.opcode == OpCode::AtomicRMW) {
                const auto input = IntegerImmediate(cursor.Take());
                if (input > static_cast<u64>(AtomicRMWOp::SubBorrow)) Reject(CompileError::UnsupportedOperand);
                kind = static_cast<AtomicRMWOp>(input);
            }
            const auto address = cursor.Take(), input = cursor.Take();
            const auto extra = compare || operation.opcode == OpCode::AtomicRMW ? cursor.Take() : CellView{};
            if (!options.private_uniforms) Shadow(false);
            Move(0, Read(address, 0));
            const auto size = width == 64 ? 3u : width == 32 ? 2u : width == 16 ? 1u : 0u;
            const u32 sf = width == 64 ? 1u << 31 : 0u;
            const auto native_width = width == 64 ? 64u : 32u;
            const auto load_exclusive = 0x085ffc00u | (size << 30) | 16u; // LDAXR[B/H/W/X] old16,[x0].
            const auto store_exclusive = 0x0800fc00u | (size << 30) | (30u << 16); // STLXR status30,data,[x0].
            const auto retry_branch = [&](u32 target) {
                const auto displacement = (target - static_cast<u32>(output.words.size())) & 0x7ffffu;
                Put(0x35000000u | (displacement << 5) | 30u); // CBNZ W30,retry; no NZCV clobber.
            };
            if (compare) {
                Move(17, Read(extra, 17, width, false, native_width));
                if (options.lse_atomics) {
                    Move(16, Read(input, 16, width, false, native_width));
                    Put(0x08e0fc00u | (size << 30) | (16u << 16) | 17u); // CASAL expected/old16,desired17,[x0].
                } else {
                    const auto retry = static_cast<u32>(output.words.size());
                    Move(30, Read(input, 30, width, false, native_width));
                    Put(load_exclusive);
                    Put(0x4a000000u | sf | (30u << 16) | (16u << 5) | 30u); // EOR expected^old.
                    const auto mismatch = output.words.size();
                    Put((width == 64 ? 0xb5000000u : 0x35000000u) | 30u);
                    Put(store_exclusive | 17u);
                    retry_branch(retry);
                    const auto done = output.words.size(); Put(0x14000000u);
                    output.words[mismatch] |= static_cast<u32>(output.words.size() - mismatch) << 5;
                    // A failed comparison still performs a write cycle.
                    // Do not bypass write permission/SMC faults with CLREX.
                    Put(store_exclusive | 16u);
                    retry_branch(retry);
                    output.words[done] |= static_cast<u32>(output.words.size() - done);
                }
            } else {
                const auto prepare = [&] {
                    if (exchange || kind != AtomicRMWOp::Neg)
                        Move(17, Read(input, 17, width, false, native_width));
                    if (kind == AtomicRMWOp::AddCarry || kind == AtomicRMWOp::SubBorrow)
                        Move(30, Read(extra, 30, 1));
                };
                if (options.lse_atomics && kind != AtomicRMWOp::Neg) {
                    prepare();
                    u32 code{};
                    if (exchange) code = 0x8000u; // SWPAL.
                    else switch (kind) {
                        case AtomicRMWOp::Add: break;
                        case AtomicRMWOp::Sub:
                            Put(0x4b000000u | sf | (17u << 16) | (31u << 5) | 17u); break;
                        case AtomicRMWOp::And:
                            Put(0x2a200000u | sf | (17u << 16) | (31u << 5) | 17u);
                            code = 0x1000u; break; // LDCLRAL with ~operand.
                        case AtomicRMWOp::Or: code = 0x3000u; break;
                        case AtomicRMWOp::Xor: code = 0x2000u; break;
                        case AtomicRMWOp::AddCarry:
                            Put(0x0b000000u | sf | (30u << 16) | (17u << 5) | 17u); break;
                        case AtomicRMWOp::SubBorrow:
                            Put(0x0b000000u | sf | (30u << 16) | (17u << 5) | 17u);
                            Put(0x4b000000u | sf | (17u << 16) | (31u << 5) | 17u); break;
                        case AtomicRMWOp::Neg: break; // Uses the shared exclusive loop below.
                    }
                    Put(0x38e00000u | (size << 30) | (17u << 16) | code | 16u);
                } else {
                    const auto retry = static_cast<u32>(output.words.size());
                    // Reload operand/carry on retry: value17 and carry30
                    // become the attempted update and store status.
                    prepare();
                    Put(load_exclusive);
                    if (!exchange) switch (kind) {
                        case AtomicRMWOp::Add: case AtomicRMWOp::AddCarry:
                            Put(0x0b000000u | sf | (17u << 16) | (16u << 5) | 17u); break;
                        case AtomicRMWOp::Sub: case AtomicRMWOp::SubBorrow:
                            Put(0x4b000000u | sf | (17u << 16) | (16u << 5) | 17u); break;
                        case AtomicRMWOp::And:
                            Put(0x0a000000u | sf | (17u << 16) | (16u << 5) | 17u); break;
                        case AtomicRMWOp::Or:
                            Put(0x2a000000u | sf | (17u << 16) | (16u << 5) | 17u); break;
                        case AtomicRMWOp::Xor:
                            Put(0x4a000000u | sf | (17u << 16) | (16u << 5) | 17u); break;
                        case AtomicRMWOp::Neg:
                            Put(0x4b000000u | sf | (16u << 16) | (31u << 5) | 17u); break;
                    }
                    if (kind == AtomicRMWOp::AddCarry || kind == AtomicRMWOp::SubBorrow)
                        Put((kind == AtomicRMWOp::AddCarry ? 0x0b000000u : 0x4b000000u) |
                                sf | (30u << 16) | (17u << 5) | 17u);
                    Put(store_exclusive | 17u);
                    retry_branch(retry);
                }
            }
            // Faultable instructions never publish their observed old
            // value into an architectural pin or the final SSA home.
            // Exclusive failures retry; only a completed RMW commits it.
            Move(destination, 16); save_result();
            if (!options.private_uniforms) Shadow(true);
            break;
        }
        case OpCode::CheckMemoryAlignment: {
            if (!options.private_uniforms) Shadow(false);
            const auto address = Read(cursor.Take(), 16);
            Immediate(17, IntegerImmediate(cursor.Take()));
            Put(0x8a000000u | (17u << 16) | (address << 5)); // AND x0,address,mask.
            Put(0xb4000040u); Put(0xd4200000u); // CBZ x0,+8; BRK on a guest fault.
            break;
        }
        case OpCode::MemoryBarrierTSO: Put(0xd5033bbf); break;
        case OpCode::MemoryStoreFence: Put(0xd5033ebf); break; // DMB ST.
        case OpCode::UniformBarrier: PublishPinned(); Put(0xd5033bbf); break;
        case OpCode::GetOperand: {
            const auto expression = *cursor.at;
            const bool word = width == 32 && expression.Kind() == OperandKind::Expression &&
                    expression.ShiftAmount() < 32;
            const auto value = Expression(cursor, destination, 17, word ? 32 : 64);
            if (word) Move(destination, value);
            else Narrow(destination, value, width);
            break;
        }
        case OpCode::BitCast: {
            const auto value = cursor.Take();
            if (value.Kind() != OperandKind::Value) Reject(CompileError::UnsupportedOperand);
            Width(View(value)); // Raw transport still has a supported scalar view.
            Move(destination, RawValue(static_cast<u32>(value.Payload()), 16)); break;
        }
        case OpCode::ZeroExtend32: case OpCode::ZeroExtend32To64: case OpCode::ZeroExtend64: case OpCode::SignExtend: {
            const auto input = cursor.Take();
            if (node == elided_consumer) { elided_consumer = {}; break; }
            const auto limit = operation.opcode == OpCode::ZeroExtend32 || operation.opcode == OpCode::ZeroExtend32To64 ? 32u : 64u;
            const bool sign = operation.opcode == OpCode::SignExtend;
            if (width == 32) {
                const auto source_bits = std::min(Width(View(input)), limit);
                const auto value = Read(input, destination, limit, false, 32);
                if (sign && source_bits < 32)
                    Put(0x13000000u | ((source_bits - 1) << 10) | (value << 5) | destination); // SXTB/SXTH W.
                else Narrow(destination, value, 32);
            } else {
                const auto value = Read(input, destination, limit, sign);
                Narrow(destination, value, width);
            }
            break;
        }
        case OpCode::Add: case OpCode::Sub: case OpCode::And: case OpCode::Or: case OpCode::Xor: case OpCode::AndNot: {
            if (node == elided_consumer) {
                cursor.Take(); cursor.Take(); elided_consumer = {}; break;
            }
            if (test_prefix && operation.opcode == OpCode::And && DeferTest(operation, cursor)) {
                cursor.Done(); return;
            }
            const auto input = cursor.Take();
            const auto operand = *cursor.at;
            const auto native_width = width == 32 ? 32u : 64u;
            if (RawAF() && !legacy_mask && operation.opcode == OpCode::And && operation.type == Type::U64 &&
                    flag_mask == all_flags && metadata.profile == FlagProfile::LogicalZeroCVA &&
                    home.last_use == home.definition_position && !home.spilled &&
                    input.Kind() == OperandKind::Value && operand.Kind() == OperandKind::Value &&
                    input.Payload() == operand.Payload() && View(input) == Type::U64 && View(operand) == Type::U64) {
                cursor.Take(); const auto value = Read(input, 16);
                nzcv_matches = 0; PrepareNZCV(all_flags);
                Put(0xf1000001u | (value << 5)); // SUBS x1,value,#0 also supplies inverted guest C=1.
                LogicalFlags(1, 64, all_flags, true, /*nzcv_ready=*/true, /*invert_carry=*/false);
                saved = true;
                break;
            }
            if (RawAF() && !legacy_mask && operation.opcode == OpCode::And &&
                    (width == 8 || width == 16) && flag_mask == all_flags &&
                    home.last_use == home.definition_position && !home.spilled &&
                    input.Kind() == OperandKind::Value && operand.Kind() == OperandKind::Value &&
                    input.Payload() == operand.Payload() && input.TypeView() == operand.TypeView()) {
                cursor.Take();
                const auto available = std::min(Width(View(input)), allocation.homes[input.Payload()].CarrierBits());
                const auto value = available >= width ? RawValue(static_cast<u32>(input.Payload()), 16, true) :
                        Read(input, 16, width);
                // There is no numeric reader: x1 is only the raw PF carrier.
                // Its sign-extended N/Z match the narrow result; its low byte
                // still supplies PF and the original clear-AF contract.
                Narrow(1, value, width, true);
                LogicalFlags(1, 64, all_flags, true);
                saved = true;
                break;
            }
            if (NativeNZCV() && operation.opcode == OpCode::And &&
                    (flag_mask == zero || flag_mask == (carry | zero)) &&
                    input.Kind() == OperandKind::Value && home.last_use == home.definition_position &&
                    (operand.Kind() == OperandKind::Immediate ||
                     (operand.Kind() == OperandKind::Value && input.Payload() == operand.Payload() &&
                      View(input) == View(operand)))) {
                auto tested_bits = std::min(width, Width(View(input)));
                tested_bits = std::min(tested_bits, allocation.homes[input.Payload()].CarrierBits());
                const auto test_width = tested_bits <= 32 ? 32u : 64u;
                const bool immediate = operand.Kind() == OperandKind::Immediate;
                const bool immediate_test = immediate || tested_bits < 32;
                u32 fields{};
                bool encodable = true;
                if (immediate) {
                    const auto literal_bits = std::min(tested_bits, Width(operand.TypeView()));
                    const auto literal = IntegerImmediate(operand) & (~u64{} >> (64 - literal_bits));
                    encodable = LogicalImmediate(literal, test_width, fields);
                } else if (immediate_test) LogicalImmediate((u64{1} << tested_bits) - 1, test_width, fields);
                if (encodable) {
                    cursor.Take();
                    const auto value = RawValue(static_cast<u32>(input.Payload()), 16, true);
                    nzcv_matches = 0; PrepareNZCV(flag_mask);
                    if (immediate_test)
                        Put((test_width == 32 ? 0x7200001fu : 0xf200001fu) | fields | (value << 5));
                    else Put((test_width == 32 ? 0x6a00001fu : 0xea00001fu) | (value << 16) | (value << 5));
                    FinishNZCV(flag_mask, true); // TST's C=0 needs the native inverted guest carry.
                    saved = true; // No numerical reader exists, including a spill reader.
                    break;
                }
            }
            if (NativeNZCV() && operation.opcode == OpCode::Sub && (width == 8 || width == 16) &&
                    !legacy_mask && (flag_mask == zero || flag_mask == (carry | zero)) &&
                    input.Kind() == OperandKind::Value && home.last_use == home.definition_position) {
                u64 literal{};
                bool shifted{}, compare{};
                if (operand.Kind() == OperandKind::Immediate) {
                    const auto bits = std::min(width, Width(operand.TypeView()));
                    literal = IntegerImmediate(operand) & (~u64{} >> (64 - bits));
                    shifted = literal > 4095 && !(literal & 4095);
                    compare = (shifted ? literal >> 12 : literal) <= 4095;
                } else if (operand.Kind() == OperandKind::Value) {
                    // Match Read's already-normalized numeric case. A pair
                    // of dirty borrowed pins can be cheaper in the aligned
                    // fallback than two cuts followed by a W comparison.
                    const auto canonical = [&](CellView value) {
                        const auto bits = std::min(width, Width(View(value)));
                        const auto& producer = source.Operations()[value.Payload()];
                        return !allocation.homes[value.Payload()].narrow_pin && Bits(producer.type) <= bits &&
                                producer.opcode != OpCode::BitCast && producer.opcode != OpCode::GetResult &&
                                producer.opcode != OpCode::GetFlags;
                    };
                    compare = canonical(input) && canonical(operand);
                }
                if (compare) {
                    cursor.Take();
                    const auto right = operand.Kind() == OperandKind::Value ? Read(operand, 17, width, false, 32) : 31u;
                    const auto left = Read(input, 16, width, false, 32);
                    nzcv_matches = 0; PrepareNZCV(flag_mask);
                    if (operand.Kind() == OperandKind::Immediate)
                        Put(0x7100001fu | (static_cast<u32>(shifted) << 22) |
                                (static_cast<u32>(shifted ? literal >> 12 : literal) << 10) | (left << 5));
                    else Put(0x6b00001fu | (right << 16) | (left << 5));
                    // Zero-extended narrow inputs have the same C/Z as CMP W.
                    // The ordinary flags path preserves N/V and the raw PF/AF.
                    ArithmeticFlags(31, flag_mask, metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                    saved = true;
                    break;
                }
            }
            if (RawAF() && operation.opcode == OpCode::Sub && (width == 8 || width == 16) &&
                    !legacy_mask && flag_mask == all_flags && input.Kind() == OperandKind::Value &&
                    operand.Kind() == OperandKind::Immediate && home.last_use == home.definition_position) {
                const auto bits = std::min(width, Width(operand.TypeView()));
                const auto literal = IntegerImmediate(operand) & (~u64{} >> (64 - bits));
                const bool negative_literal = literal & (u64{1} << (width - 1));
                const auto magnitude = negative_literal ? (u64{1} << width) - literal : literal;
                const bool shifted = magnitude > 4095 && !(magnitude & 4095);
                const auto field = shifted ? magnitude >> 12 : magnitude;
                if (field <= 4095) {
                    cursor.Take();
                    const auto input_bits = std::min(Width(View(input)), allocation.homes[input.Payload()].CarrierBits());
                    const auto left = input_bits >= width ? RawValue(static_cast<u32>(input.Payload()), 16, true) :
                            Read(input, 16, width, false, 32);
                    Put(0x13000000u | ((width - 1) << 10) | (left << 5) | 16u); // SXTB/SXTH W16 also cuts dirty pins.
                    // Raw AF only observes bit4. A W write also clears the TEST tag.
                    Put((literal & 16 ? 0x2a2003e0u : 0x2a0003e0u) | (16u << 16) | 29u);
                    nzcv_matches = 0; PrepareNZCV(flag_mask);
                    Put((negative_literal ? 0x31000000u : 0x71000000u) |
                            (static_cast<u32>(shifted) << 22) | (static_cast<u32>(field) << 10) | (16u << 5) | 1u);
                    // Signed narrow subtraction fits n+1 bits. SETF restores
                    // N/Z/V from that result and preserves the correct unsigned C.
                    Put((width == 8 ? 0x3a00080du : 0x3a00480du) | (1u << 5));
                    ArithmeticFlags(1, flag_mask, metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                    saved = true; // The raw PF carrier's high bits are not a numerical result.
                    break;
                }
            }
            if (operation.opcode == OpCode::And && operand.Kind() == OperandKind::Immediate &&
                    (width == 8 || width == 16)) {
                const auto literal_bits = std::min(width, Width(operand.TypeView()));
                const auto literal = IntegerImmediate(operand) & ((u64{1} << literal_bits) - 1);
                const auto repeated = width == 8 ? literal * 0x01010101u : literal * 0x00010001u;
                u32 fields{};
                if (LogicalImmediate(repeated, 32, fields)) {
                    cursor.Take();
                    // Repetition only encodes the mask. Read still clears
                    // every input bit above the logical width, including
                    // borrowed pins and raw transports. The W result stays
                    // canonical, without a separate result cut.
                    const auto left = Read(input, 16, width, false, 32);
                    Put(0x12000000u | fields | (left << 5) | destination);
                    if (flag_mask) {
                        save_result(); LogicalFlags(destination, width, flag_mask, !legacy_mask);
                    }
                    break;
                }
            }
            if (Logical(operation.opcode) && operand.Kind() == OperandKind::Immediate && width >= 32) {
                auto literal = IntegerImmediate(operand);
                const auto literal_width = std::min(Width(operand.TypeView()), width);
                if (literal_width < 64) literal &= (u64{1} << literal_width) - 1;
                if (operation.opcode == OpCode::AndNot) literal = ~literal;
                u32 fields;
                if (LogicalImmediate(literal, width, fields)) {
                    cursor.Take(); const auto left = Read(input, 16, 64, false, native_width);
                    const bool set_nzcv = operation.opcode == OpCode::And && (flag_mask & (negative | zero));
                    auto opcode = operation.opcode == OpCode::Or ? 0xb2000000u :
                            operation.opcode == OpCode::Xor ? 0xd2000000u : set_nzcv ? 0xf2000000u : 0x92000000u;
                    if (width == 32) opcode &= ~(1u << 31);
                    if (set_nzcv) { nzcv_matches = 0; PrepareNZCV(flag_mask); }
                    Put(opcode | fields | (left << 5) | destination);
                    if (flag_mask) {
                        save_result(); LogicalFlags(destination, width, flag_mask, !legacy_mask, set_nzcv);
                    }
                    break;
                }
            }
            if (Arithmetic(operation.opcode) && operand.Kind() == OperandKind::Immediate &&
                (width >= 32 || ((width == 8 || width == 16) && !flag_mask))) {
                auto literal = IntegerImmediate(operand);
                const auto literal_width = std::min(Width(operand.TypeView()), width);
                if (literal_width < 64) literal &= (u64{1} << literal_width) - 1;
                bool subtract = operation.opcode == OpCode::Sub;
                if (!flag_mask && (literal & (u64{1} << (width - 1)))) {
                    literal = (-literal) & (~u64{} >> (64 - width)); subtract = !subtract;
                }
                const bool shifted = literal > 4095 && !(literal & 4095);
                const auto field = shifted ? literal >> 12 : literal;
                if (field <= 4095) {
                    cursor.Take();
                    // A final narrow result makes the low-width sum modulo
                    // 2^width. Borrow only when the actual read is at least
                    // that wide; Bool/narrower views still use numeric Read.
                    const bool raw_narrow = width < 32 && input.Kind() == OperandKind::Value &&
                            std::min(Width(View(input)), allocation.homes[input.Payload()].CarrierBits()) >= width;
                    const auto left = raw_narrow ? RawValue(static_cast<u32>(input.Payload()), 16, true) :
                            Read(input, 16, 64, false, width <= 32 ? 32u : 64u);
                    if (flag_mask & auxiliary) {
                        // ArithmeticFlags consumes only (left ^ right).bit4.
                        // Capture it before a destination pin overwrites left.
                        if (RawAF()) {
                            if (literal & 16) Put(0x521c0000u | (left << 5) | 29u); // EOR W29,Wleft,#16.
                            else Put(0x2a0003e0u | (left << 16) | 29u);
                        } else if (literal & 16) Put(0xd27c0000u | (left << 5)); // EOR x0,left,#16.
                        else Move(0, left);
                    }
                    auto opcode = subtract ? 0xd1000000u : 0x91000000u;
                    if (flag_mask & nzcv_flags) {
                        opcode |= 1u << 29; nzcv_matches = 0; PrepareNZCV(flag_mask);
                    }
                    if (width <= 32) opcode &= ~(1u << 31);
                    Put(opcode | (static_cast<u32>(shifted) << 22) | (static_cast<u32>(field) << 10) | (left << 5) | destination);
                    const bool partial_byte_store = !flag_mask && operation.type == Type::U8 &&
                            metadata == OperationMetadata{} && PartialByteStore(operation, home);
                    if (width < 32 && !partial_byte_store) Narrow(destination, destination, width);
                    if (flag_mask) { save_result(); ArithmeticFlags(destination, flag_mask,
                            subtract && metadata.profile == FlagProfile::ArithmeticCanonicalCarry); }
                    break;
                }
            }
            const bool logical_nzcv = operation.opcode == OpCode::And && width >= 32 &&
                    (flag_mask & (negative | zero));
            const bool set_nzcv = logical_nzcv || (Arithmetic(operation.opcode) && (flag_mask & nzcv_flags));
            const bool aligned = set_nzcv && width < 32;
            const bool zero_right = operand.Kind() == OperandKind::Immediate &&
                    !(IntegerImmediate(operand) & (~u64{} >> (64 - std::min(width, Width(operand.TypeView())))));
            unsigned right, left;
            bool physical_logical{};
            if (width < 32 && (Logical(operation.opcode) || (aligned && (width == 8 || width == 16))) &&
                    (operand.Kind() == OperandKind::Value || operand.Kind() == OperandKind::Immediate)) {
                const auto read_low = [&](CellView value, unsigned scratch) {
                    if (value.Kind() == OperandKind::Value) {
                        const auto& input_home = allocation.homes[value.Payload()];
                        // Aligned UBFIZ and the shifted W ALU cut their own inputs.
                        if ((aligned || input_home.narrow_pin) && std::min(Width(View(value)), input_home.CarrierBits()) >= width) {
                            physical_logical = true;
                            return RawValue(static_cast<u32>(value.Payload()), scratch, true);
                        }
                    }
                    return Read(value, scratch);
                };
                cursor.Take(); right = zero_right ? 31u : read_low(operand, 17); left = read_low(input, 16);
            } else {
                if (zero_right) { cursor.Take(); right = 31; }
                else right = Expression(cursor, 17, 16, native_width);
                left = Read(input, 16, 64, false, native_width);
            }
            // Ordinary narrow reads already supplied canonical integers.
            // A borrowed physical input instead needs one result cut;
            // compound expressions and wider reads retain theirs too.
            const bool normalized_logical = !physical_logical && width < 32 && Logical(operation.opcode) &&
                    (operand.Kind() == OperandKind::Value || operand.Kind() == OperandKind::Immediate) &&
                    Width(View(input)) <= width && Width(View(operand)) <= width;
            const bool partial_byte_store = physical_logical && RawAF() && !legacy_mask &&
                    operation.opcode == OpCode::Xor && flag_mask == all_flags &&
                    metadata.profile == FlagProfile::LogicalZeroCVA && PartialByteStore(operation, home);
            if (flag_mask & auxiliary && Arithmetic(operation.opcode))
                Put((RawAF() ? 0x4a00001du : 0xca000000u) | (right << 16) | (left << 5));
            if (aligned) {
                // A W shift drops bits above the logical arithmetic width;
                // the RHS shift fits the actual ADDS/SUBS operand field.
                Put(0x53000000u | (width << 16) | ((width - 1) << 10) | (left << 5) | 16u);
                left = 16;
            }
            auto opcode = operation.opcode == OpCode::Add ? 0x8b000000u
                    : operation.opcode == OpCode::Sub ? 0xcb000000u
                    : operation.opcode == OpCode::And ? 0x8a000000u
                    : operation.opcode == OpCode::Or ? 0xaa000000u
                    : operation.opcode == OpCode::Xor ? 0xca000000u : 0x8a200000u;
            if (set_nzcv) opcode |= (logical_nzcv ? 3u : 1u) << 29;
            if (width == 32 || aligned) opcode &= ~(1u << 31);
            if (set_nzcv) { nzcv_matches = 0; PrepareNZCV(flag_mask); }
            Put(opcode | (right << 16) | (aligned ? (32 - width) << 10 : 0u) | (left << 5) | destination);
            if (aligned) Extract(destination, destination, 32 - width, width);
            // Here the only numeric reader is the adjacent byte insertion;
            // it and LogicalFlags' SXTB each cut their own actual low eight bits.
            else if (width != 32 && !normalized_logical && !partial_byte_store) Narrow(destination, destination, width);
            if (flag_mask) {
                save_result();
                if (Arithmetic(operation.opcode)) ArithmeticFlags(destination, flag_mask,
                        operation.opcode == OpCode::Sub && metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                else LogicalFlags(destination, width, flag_mask, !legacy_mask, logical_nzcv);
            }
            break;
        }
        case OpCode::Neg: {
            auto value = Read(cursor.Take(), 16, 64, false, width == 32 ? 32 : 64);
            if (flag_mask & auxiliary) {
                if (RawAF()) Put(0x2a0003e0u | (value << 16) | 29u);
                else Move(0, value);
            }
            const bool set_nzcv = flag_mask & (carry | overflow | zero | negative);
            const bool aligned = set_nzcv && width < 32;
            if (aligned) { Narrow(16, value, width); ShiftLeft(16, 16, 32 - width); value = 16; }
            auto opcode = set_nzcv ? (width <= 32 ? 0x6b000000u : 0xeb000000u) :
                    width == 32 ? 0x4b000000u : 0xcb000000u;
            if (set_nzcv) { nzcv_matches = 0; PrepareNZCV(flag_mask); }
            Put(opcode | (value << 16) | (31u << 5) | destination);
            if (aligned) Extract(destination, destination, 32 - width, width);
            else if (width != 32) Narrow(destination, destination, width);
            if (flag_mask) {
                save_result(); ArithmeticFlags(destination, flag_mask,
                        metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
            }
            break;
        }
        case OpCode::Adc: case OpCode::Sbb: {
            RequireFlags();
            if (RawAF()) {
                const bool subtract = operation.opcode == OpCode::Sbb;
                const auto input = cursor.Take();
                const auto native_width = width == 32 ? 32u : 64u;
                auto right = Expression(cursor, 17, 16, native_width),
                     left = Read(input, 16, 64, false, native_width);
                if (width < 32) {
                    Narrow(16, left, width); Narrow(17, right, width);
                    left = 16; right = 17;
                }
                if (flag_mask & auxiliary)
                    Put(0x4a00001du | (right << 16) | (left << 5));
                if (width < 32 && (flag_mask & overflow)) {
                    Put(0xca000000u | (right << 16) | (left << 5));
                    Move(30, left);
                }
                const bool host_flags = width >= 32 && (flag_mask & nzcv_flags);
                if (host_flags) PrepareNZCV(flag_mask);
                if (!subtract) Put(0xd500401fu); // ADC consumes guest C, rather than its native inverse.
                auto opcode = subtract ? 0xda000000u : 0x9a000000u;
                if (host_flags) opcode |= 1u << 29;
                if (width == 32) opcode &= ~(1u << 31);
                Put(opcode | (right << 16) | (left << 5) | destination);
                if (!subtract && !host_flags) Put(0xd500401fu);
                if (width < 32) {
                    // Non-S ADC/SBC preserve incoming NZCV. Publish only
                    // the requested narrow fields, without a packed bridge.
                    if (flag_mask & carry) {
                        Extract(17, destination, subtract ? 63 : width, 1);
                        Rmif(17, 63, 2);
                        if (!subtract || metadata.profile == FlagProfile::ArithmeticCanonicalCarry) Put(0xd500401fu);
                    }
                    if (flag_mask & overflow) {
                        Put(0xca000000u | (destination << 16) | (30u << 5) | 30u);
                        Put((subtract ? 0x8a000000u : 0x8a200000u) | (30u << 5) | 30u);
                        Extract(17, 30, width - 1, 1); Rmif(17, 0, 1);
                    }
                    Narrow(destination, destination, width); save_result();
                    ArithmeticFlags(destination, flag_mask & (auxiliary | parity), false);
                    LogicalFlags(destination, width, flag_mask & (negative | zero), false);
                } else if (flag_mask) {
                    save_result(); ArithmeticFlags(destination, flag_mask,
                            subtract && metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
                }
                break;
            }
            const bool native = NativeNZCV();
            PackNZCV();
            options.native_nzcv = false; // The existing narrow emitter also owns x30.
            nzcv_matches = 0;
            const bool subtract = operation.opcode == OpCode::Sbb;
            const auto input = cursor.Take();
            const auto right = Expression(cursor, 17, 16), left = Read(input, 16);
            if (subtract) {
                Put(0xd2630020u); // EOR x0,x1,#(1<<29).
                Put(0xd51b4200u); // SBC consumes !borrow in host C.
            } else Put(0xd51b4201u);
            if ((flag_mask & auxiliary) || (width < 32 && (flag_mask & overflow)))
                Put(0xca000000u | (right << 16) | (left << 5));
            if (width < 32 && (flag_mask & overflow)) Move(30, left);
            auto opcode = subtract ? 0xda000000u : 0x9a000000u;
            const bool host_flags = width >= 32 && (flag_mask & (carry | overflow | zero | negative));
            if (host_flags) opcode |= 1u << 29;
            if (width == 32) opcode &= ~(1u << 31);
            Put(opcode | (right << 16) | (left << 5) | destination);
            if (width < 32 && flag_mask) {
                if (flag_mask & carry) {
                    Extract(17, destination, subtract ? 63 : width, 1);
                    if (subtract && metadata.profile != FlagProfile::ArithmeticCanonicalCarry) {
                        Put(0xaa2003e0u | (17u << 16) | 17u); // MVN; only bit zero is inserted.
                    }
                    Insert(1, 17, 29, 1);
                }
                if (flag_mask & overflow) {
                    Put(0xca000000u | (destination << 16) | (30u << 5) | 30u);
                    Put((subtract ? 0x8a000000u : 0x8a200000u) | (0u << 16) | (30u << 5) | 30u);
                    Extract(17, 30, width - 1, 1); Insert(1, 17, 28, 1);
                }
                if (flag_mask & auxiliary) {
                    Put(0xca000000u | (destination << 16)); Extract(0, 0, 4, 1); Insert(1, 0, 26, 1);
                }
                Narrow(destination, destination, width); save_result();
                LogicalFlags(destination, width, flag_mask & (zero | negative | parity), false);
            } else {
                Narrow(destination, destination, width);
                if (flag_mask) { save_result(); ArithmeticFlags(destination, flag_mask,
                        subtract && metadata.profile == FlagProfile::ArithmeticCanonicalCarry); }
            }
            nzcv_matches = 0; // Narrow C/V are synthesized separately; keep this case conservative.
            options.native_nzcv = native;
            RestoreNZCV();
            break;
        }
        case OpCode::Div128: {
            const auto high = cursor.Take(), low = cursor.Take(), divisor = cursor.Take();
            const auto sign = IntegerImmediate(cursor.Take()); const auto bits = Width(View(high));
            if ((bits != 8 && bits != 16 && bits != 32 && bits != 64) ||
                Width(View(low)) != bits || Width(View(divisor)) != bits || sign > 1)
                Reject(CompileError::UnsupportedOperand);
            Shadow(false);
            PublishPinned();
            HelperFPEnvironment(false);
            Move(0, Read(high, 0)); Move(16, Read(low, 16)); Move(17, Read(divisor, 17));
            Immediate(30, DivisionHelper(bits, sign != 0)); Put(0xd63f03c0u); // BLR x30.
            HelperFPEnvironment(true);
            nzcv_matches = 0;
            Memory(false, 64, 16, 31, home.extra_slot - 1);
            Move(destination, 0); RestoreHelperNZCV(); break;
        }
        case OpCode::Div128Remainder: {
            const auto owner = cursor.Take();
            if (owner.Kind() != OperandKind::Value || source.Operations()[owner.Payload()].opcode != OpCode::Div128)
                Reject(CompileError::UnsupportedOperand);
            Memory(true, 64, destination, 31, allocation.homes[owner.Payload()].extra_slot - 1); break;
        }
        case OpCode::Cpuid: {
            const auto leaf = cursor.Take(), subleaf = cursor.Take();
            const auto callback = IntegerImmediate(cursor.Take());
            if (!callback) Reject(CompileError::UnsupportedOperand);
            Shadow(false);
            PublishPinned();
            Put(0xd5033bbfu); // DMB ISH: prior guest memory precedes the serializing query.
            HelperFPEnvironment(false);
            Move(0, Read(leaf, 0)); Move(16, Read(subleaf, 16)); Immediate(17, callback);
            Immediate(30, CpuidHelper()); Put(0xd63f03c0u); // BLR x30.
            HelperFPEnvironment(true);
            Put(0xd5033fdfu); // ISB: following instructions start after the query.
            nzcv_matches = 0;
            Memory(false, 64, 16, 31, home.extra_slot - 1);
            Move(destination, 0); RestoreHelperNZCV(); break;
        }
        case OpCode::CpuidUpper: {
            const auto owner = cursor.Take();
            if (owner.Kind() != OperandKind::Value || source.Operations()[owner.Payload()].opcode != OpCode::Cpuid)
                Reject(CompileError::UnsupportedOperand);
            Memory(true, 64, destination, 31, allocation.homes[owner.Payload()].extra_slot - 1); break;
        }
        case OpCode::VecFCvtIntToFloat: case OpCode::VecFCvtFloatToInt:
            EmitVector(operation, cursor, destination); break;
        case OpCode::Mul: {
            const auto input = cursor.Take();
            const auto operand = *cursor.at;
            if (operation.type == Type::U32 && metadata == OperationMetadata{} && !home.spilled &&
                    node.value + 1 < source.Operations().size()) {
                const NodeId consumer{node.value + 1};
                const auto& next = source.Operations()[consumer.value];
                const auto& next_home = allocation.homes[consumer.value];
                if (next.opcode == OpCode::Add && next.type == Type::U32 && next.block == operation.block &&
                        next.Metadata() == OperationMetadata{} && !next_home.spilled &&
                        home.last_use == next_home.definition_position && next_home.reg != deferred_parity) {
                    const auto addends = source.Operands(consumer); auto at = addends.begin();
                    if (at != addends.end()) {
                        const auto first = *at++;
                        if (at != addends.end()) {
                            const auto second = *at++;
                            const bool first_product = first.Kind() == OperandKind::Value && first.Payload() == node.value;
                            const bool second_product = second.Kind() == OperandKind::Value && second.Payload() == node.value;
                            const auto accumulator = first_product ? second : first;
                            const auto register_input = [&](CellView value) {
                                return value.Kind() == OperandKind::Value && View(value) == Type::U32 &&
                                        !allocation.homes[value.Payload()].spilled &&
                                        allocation.homes[value.Payload()].CarrierBits() >= 32;
                            };
                            NodeId publication{consumer.value + 1};
                            if (publication.value < source.Operations().size() &&
                                    source.Operations()[publication.value].opcode == OpCode::ClearFlags) ++publication.value;
                            if (at == addends.end() && first_product != second_product &&
                                    View(first_product ? first : second) == Type::U32 &&
                                    register_input(input) && register_input(operand) && register_input(accumulator) &&
                                    !LegacyMask(consumer, publication)) {
                                cursor.Take();
                                const auto left = Read(input, 16, 64, false, 32);
                                const auto right = Read(operand, 17, 64, false, 32);
                                const auto addend = Read(accumulator, 16, 64, false, 32);
                                Put(0x1b000000u | (right << 16) | (addend << 10) | (left << 5) | next_home.reg);
                                elided_consumer = consumer;
                                saved = true;
                                break;
                            }
                        }
                    }
                }
            }
            const auto native_width = width <= 32 ? 32u : 64u;
            const auto right = Expression(cursor, 17, 16, native_width), left = Read(input, 16, 64, false, native_width);
            Put((width <= 32 ? 0x1b007c00u : 0x9b007c00u) | (right << 16) | (left << 5) | destination);
            if (width < 32) Narrow(destination, destination, width);
            break;
        }
        case OpCode::MulHigh: {
            const auto left = RawScalar(cursor.Take(), 16), right = RawScalar(cursor.Take(), 17);
            const auto sign = IntegerImmediate(cursor.Take());
            if (sign > 1) Reject(CompileError::UnsupportedOperand);
            Put((sign ? 0x9b407c00u : 0x9bc07c00u) | (right << 16) | (left << 5) | destination);
            Narrow(destination, destination, width); break;
        }
        case OpCode::BitExtract: case OpCode::BitClear: {
            const auto input = cursor.Take();
            const auto first = IntegerImmediate(cursor.Take()), count = IntegerImmediate(cursor.Take());
            Field(first, count);
            if (!count && operation.opcode == OpCode::BitExtract) Immediate(destination, 0);
            else {
                const bool extract = operation.opcode == OpCode::BitExtract;
                const auto value = RawScalar(input, 16, extract ? 1u : 0u); // Extract clips its carrier range below.
                if (extract) {
                    // Borrowed pin bits above the logical carrier are zero;
                    // UBFX supplies both the extraction and result narrowing.
                    const auto carrier_bits = allocation.homes[input.Payload()].CarrierBits();
                    const auto remaining = first < carrier_bits ? u64{carrier_bits} - first : 0;
                    const auto actual_count = std::min({count, u64{width}, remaining});
                    if (actual_count) Extract(destination, value, static_cast<unsigned>(first),
                                              static_cast<unsigned>(actual_count));
                    else Immediate(destination, 0);
                } else {
                    Move(destination, value);
                    if (count) Insert(destination, 31, static_cast<unsigned>(first), static_cast<unsigned>(count));
                    Narrow(destination, destination, width);
                }
            }
            break;
        }
        case OpCode::BitInsert: {
            const auto input = cursor.Take(), insertion = cursor.Take();
            const auto first = IntegerImmediate(cursor.Take()), count = IntegerImmediate(cursor.Take());
            Field(first, count);
            const bool word_insert = width == 32 && count && first < 32 && count <= 32 - first;
            const auto value = RawScalar(input, 16, word_insert ? 32u : 0u);
            Width(View(insertion));
            auto other = count ? RawScalar(insertion, 17, word_insert ? static_cast<unsigned>(count) : 0u) : 31;
            if (other == destination && value != destination) {
                Move(17, other); // Preserve the whole raw source before copying the parent over its last-use home.
                other = 17;
            }
            Move(destination, value);
            if (count) Insert(destination, other, static_cast<unsigned>(first), static_cast<unsigned>(count), word_insert ? 32u : 64u);
            if (!word_insert) Narrow(destination, destination, width);
            break;
        }
        case OpCode::Crc32c: {
            if (!options.crc32c) Reject(CompileError::UnsupportedOpcode);
            const auto accumulator = cursor.Take(), input = cursor.Take();
            const auto bits = IntegerImmediate(cursor.Take());
            unsigned size{};
            switch (bits) {
                case 8: break;
                case 16: size = 1; break;
                case 32: size = 2; break;
                case 64: size = 3; break;
                default: Reject(CompileError::UnsupportedOperand);
            }
            const auto initial = Read(accumulator, 16, 32, false, 32);
            const auto data = Read(input, 17, static_cast<unsigned>(bits), false, bits == 64 ? 64 : 32);
            // CRC32CB/CH/CW/CX: W accumulator and result, with X data
            // only for the 64-bit form. No NZCV or complement operation.
            Put(0x1ac05000u | (size << 10) | (size == 3 ? 0x80000000u : 0) |
                    (data << 16) | (initial << 5) | destination);
            break;
        }
        case OpCode::ByteSwap: {
            const auto input = RawScalar(cursor.Take(), 16);
            const auto swapped = IntegerImmediate(cursor.Take());
            if (swapped != 16 && swapped != 32 && swapped != 64) Reject(CompileError::UnsupportedOperand);
            Put((swapped == 16 ? 0x5ac00400u : swapped == 32 ? 0x5ac00800u : 0xdac00c00u) | (input << 5) | destination);
            const auto kept = std::min(width, static_cast<unsigned>(swapped));
            if (kept < 32 || (swapped == 64 && kept < 64)) Narrow(destination, destination, kept);
            break;
        }
        case OpCode::PopCount: {
            const auto input = RawScalar(cursor.Take(), 16);
            // Both scalar and vector allocation reserve v0 as scratch;
            // it is never a value home.
            Put(0x9e670000u | (input << 5)); // FMOV d0,Xn (raw bits, no FP conversion).
            Put(0x0e205800u); // CNT v0.8b,v0.8b
            Put(0x0e31b800u); // ADDV b0,v0.8b
            Put(0x0e013c00u | destination); // UMOV Wd,v0.b[0]
            if (width < 7) Narrow(destination, destination, width);
            break;
        }
        case OpCode::CountLeadingZeros32: case OpCode::CountLeadingZeros64:
        case OpCode::CountTrailingZeros32: case OpCode::CountTrailingZeros64: {
            const bool wide = operation.opcode == OpCode::CountLeadingZeros64 || operation.opcode == OpCode::CountTrailingZeros64;
            const bool trailing = operation.opcode == OpCode::CountTrailingZeros32 || operation.opcode == OpCode::CountTrailingZeros64;
            const auto input = cursor.Take();
            if (Width(View(input)) != (wide ? 64u : 32u)) Reject(CompileError::UnsupportedType);
            auto value = RawScalar(input, 16);
            if (trailing) { Put((wide ? 0xdac00000u : 0x5ac00000u) | (value << 5) | destination); value = destination; }
            Put((wide ? 0xdac01000u : 0x5ac01000u) | (value << 5) | destination);
            if (width < (wide ? 7u : 6u)) Narrow(destination, destination, width);
            break;
        }
        case OpCode::CondSet: case OpCode::LocalCondSet: {
            RequireFlags(); const auto condition = Condition(cursor.Take());
            if (home.terminal_only) { terminal_condition = condition; break; }
            if (condition >= 14) Immediate(destination, 1);
            else {
                EnsureNZCV(condition);
                const bool flip = NativeNZCV() && (condition == 8 || condition == 9);
                const auto host = NativeNZCV() && (condition == 2 || condition == 3) ? condition ^ 1 : condition;
                if (flip) Put(0xd500401fu);
                Put(0x9a9f07e0u | ((host ^ 1) << 12) | destination);
                if (flip) Put(0xd500401fu);
            }
            break;
        }
        case OpCode::CondSelect: {
            RequireFlags(); const auto condition = Condition(cursor.Take());
            const auto native_width = width == 32 ? 32u : 64u;
            const auto yes = Read(cursor.Take(), 16, 64, false, native_width),
                       no = Read(cursor.Take(), 17, 64, false, native_width);
            if (condition >= 14) { Narrow(destination, yes, width); break; }
            EnsureNZCV(condition);
            const bool flip = NativeNZCV() && (condition == 8 || condition == 9);
            const auto host = NativeNZCV() && (condition == 2 || condition == 3) ? condition ^ 1 : condition;
            if (flip) Put(0xd500401fu);
            Put((width == 32 ? 0x1a800000u : 0x9a800000u) |
                    (no << 16) | (host << 12) | (yes << 5) | destination);
            if (flip) Put(0xd500401fu);
            if (width != 32) Narrow(destination, destination, width);
            break;
        }
        case OpCode::Select: {
            SnapshotCompareNZCV();
            const auto check = Read(cursor.Take(), 0);
            const auto native_width = width == 32 ? 32u : 64u;
            const auto yes = Read(cursor.Take(), 16, 64, false, native_width),
                    no = Read(cursor.Take(), 17, 64, false, native_width);
            nzcv_matches = 0;
            Put(0xf100001fu | (check << 5));
            Put((width == 32 ? 0x1a800000u : 0x9a800000u) |
                    (no << 16) | (1u << 12) | (yes << 5) | destination);
            RestoreCompareNZCV();
            if (width != 32) Narrow(destination, destination, width);
            break;
        }
        case OpCode::Not: {
            const auto input = cursor.Take();
            if (cursor.at == cursor.end) Predicate(destination, Read(input, 16), true);
            else { const auto value = Expression(cursor, 16, 17); Put(0xaa2003e0u | (value << 16) | destination); }
            Narrow(destination, destination, width); break;
        }
        case OpCode::TestZero: case OpCode::TestNotZero:
            Predicate(destination, Read(cursor.Take(), 16), operation.opcode == OpCode::TestZero); break;
        case OpCode::TestBit: {
            const auto input = cursor.Take(); const auto bit = IntegerImmediate(cursor.Take());
            if (bit >= 64) Reject(CompileError::UnsupportedOperand);
            if (bit >= Width(View(input))) Immediate(destination, 0);
            else Extract(destination, Read(input, 16), static_cast<unsigned>(bit), 1);
            break;
        }
        case OpCode::LslImm: case OpCode::LslValue: case OpCode::LsrImm: case OpCode::LsrValue:
        case OpCode::AsrImm: case OpCode::AsrValue: case OpCode::RorImm: case OpCode::RorValue: {
            const auto input = cursor.Take(), amount = cursor.Take();
            const bool asr = operation.opcode == OpCode::AsrImm || operation.opcode == OpCode::AsrValue;
            const bool ror = operation.opcode == OpCode::RorImm || operation.opcode == OpCode::RorValue;
            const bool lsl = operation.opcode == OpCode::LslImm || operation.opcode == OpCode::LslValue;
            if (ror && width != 32 && width != 64) Reject(CompileError::UnsupportedType);
            if (amount.Kind() == OperandKind::Immediate) {
                const auto count = static_cast<unsigned>(IntegerImmediate(amount) & (width - 1) &
                        ((u64{1} << std::min(Width(amount.TypeView()), 6u)) - 1));
                const auto value = Read(input, 16, width, asr, width == 32 ? 32 : 64);
                if (ror) {
                    Put((width == 32 ? 0x13800000u : 0x93c00000u) | (value << 16) |
                            (count << 10) | (value << 5) | destination);
                } else if (asr) {
                    Put((width == 32 ? 0x13000000u : 0x93400000u) | (count << 16) |
                            ((width == 32 ? 31u : 63u) << 10) | (value << 5) | destination);
                    if (width < 32) Narrow(destination, destination, width);
                } else {
                    const auto register_width = width == 32 ? 32u : 64u;
                    Put((width == 32 ? 0x53000000u : 0xd3400000u) |
                            ((lsl ? (register_width - count) % register_width : count) << 16) |
                            ((lsl ? width - count - 1 : width - 1) << 10) | (value << 5) | destination);
                }
                break;
            }
            auto count = Read(amount, 17);
            // Scalar shift counts reduce modulo the actual result width.
            unsigned count_bits{}; for (auto n = width; n > 1; n >>= 1) ++count_bits;
            if (!count_bits) { Immediate(17, 0); count = 17; }
            else { Narrow(17, count, count_bits); count = 17; }
            const auto value = Read(input, 16, width, asr, width == 32 ? 32 : 64);
            auto opcode = asr ? 0x9ac02800u : ror ? 0x9ac02c00u : lsl ? 0x9ac02000u : 0x9ac02400u;
            if (width == 32) opcode &= ~(1u << 31);
            Put(opcode | (count << 16) | (value << 5) | destination);
            if (width < 32) Narrow(destination, destination, width);
            break;
        }
        default: Reject(CompileError::UnsupportedOpcode);
    }
    cursor.Done();
    if (!saved) save_result();
}

void Writer::Branch(BlockId target) {
    if (target.value >= labels.size()) Reject(CompileError::InvalidProgram);
    fixups.push_back({static_cast<u32>(output.words.size()), target.value}); Put(0x14000000u);
}

bool Writer::ShortBranch(u32 opcode, BlockId target) {
    // B.cond and CBZ/CBNZ share imm19. A forward edge borrows the original
    // fixup list; only a genuine final range miss asks for long emission.
    if (labels[target.value] == UINT32_MAX) {
        if (long_forward_branches) return false;
        fixups.push_back({static_cast<u32>(output.words.size()), target.value});
        Put(opcode); return true;
    }
    const auto distance = static_cast<std::int64_t>(labels[target.value]) -
            static_cast<std::int64_t>(output.words.size());
    if (distance < -(std::int64_t{1} << 18) || distance >= (std::int64_t{1} << 18)) return false;
    Put(opcode | ((static_cast<u32>(distance) & 0x7ffffu) << 5));
    return true;
}

void Writer::TailJump(std::uintptr_t destination, bool link, u64 target_pc, u32 conditional_head) {
    if (!link && destination == options.dispatcher && dispatcher_literal != UINT32_MAX) {
        const auto distance = static_cast<std::int64_t>(dispatcher_literal) -
                static_cast<std::int64_t>(output.words.size());
        if (distance >= -(INT64_C(1) << 18) && distance < (INT64_C(1) << 18)) {
            Put(0x58000010u | ((static_cast<u32>(distance) & 0x7ffffu) << 5));
            Put(0xd61f0200u); // LDR x16,immutable dispatcher DATA; BR x16.
            return;
        }
    }
    // CodeArena aligns entries. Keep DATA aligned without executing padding
    // before a resolved branch; the BR also skips padding on the far path.
    // A conditional head reaches this cold gate only when unresolved/far.
    // Its zero path already transfers elsewhere, so alignment here is cold.
    if (conditional_head != UINT32_MAX && (output.words.size() & 1)) Put(0xd503201fu);
    const auto branch = static_cast<u32>(output.words.size());
    const auto padding = branch & 1u;
    const auto pointer = branch + 2 + padding;
    Put(0x58000010u | ((2 + padding) << 5)); Put(0xd61f0200u);
    if (padding) Put(0xd503201fu);
    Put(static_cast<u32>(destination)); Put(static_cast<u32>(static_cast<u64>(destination) >> 32));
    if (!link && destination == options.dispatcher) dispatcher_literal = pointer;
    if (link) {
        output.links.push_back({pointer, conditional_head == UINT32_MAX ? branch : conditional_head, target_pc});
        if (conditional_head != UINT32_MAX) {
            const auto distance = branch - conditional_head;
            output.words[conditional_head] = (output.words[conditional_head] & ~(0x7ffffu << 5)) | (distance << 5);
        }
        // Only this self link can carry the local TEST tag. Unlinking must
        // complete its actual next value before the ordinary dispatcher.
        if (source.ScopeKind() == Scope::Block && deferred_test.bits == 64 && target_pc == options.entry_pc)
            CompleteDeferredTest();
        // This edge is already outside the source unit's spill frame.
        // Resolved B26/far links bypass it; only cold dispatch needs PC.
        Immediate(0, target_pc); PCMemory(false);
        TailJump(destination); // No second relocation or frame retirement.
    }
}

void Writer::DynamicDispatch() {
    if (options.dispatch_mode == DispatchMode::Callback) { TailJump(options.dispatcher); return; }
    // The source frame is already retired and x0 holds the complete target.
    // Each dynamic site gets its own indirect BR while sharing the original
    // table, probe rules and cold resolver. No flags or pinned state change.
    const auto count = [&](unsigned offset) {
        if (options.dispatch_mode != DispatchMode::InlineCounted) return;
        Memory(true, 64, 4, 28, offset / 8);
        Put(0x91000484u); // ADD x4,x4,#1, preserving guest NZCV.
        Memory(false, 64, 4, 28, offset / 8);
    };
    count(abi::ContextDispatchesOffset);
    Put(0xa9400000u | ((abi::ContextSlotsOffset / 8) << 15) | (6u << 10) | (28u << 5) | 2u);
    u32 no_slots = UINT32_MAX;
    if (!options.callable_directory) { no_slots = static_cast<u32>(output.words.size()); Put(0xb4000002u); }
    Put(0xca400000u | (abi::DispatchHashShift << 10) | 5u); // EOR x5,x0,x0,LSR #hash.
    Put(0x8a000000u | (6u << 16) | (5u << 5) | 5u);
    const auto probe = static_cast<u32>(output.words.size());
    count(abi::ContextProbesOffset);
    Put(0x8b000000u | (5u << 16) | (abi::DispatchSlotShift << 10) | (2u << 5) | 3u);
    Put(0xa9400000u | (16u << 10) | (3u << 5) | 4u);
    Put(0xcb000000u | (4u << 5) | 4u); // SUB x4,tag,x0, non-S.
    const auto mismatch = static_cast<u32>(output.words.size()); Put(0xb5000004u);
    u32 pending = UINT32_MAX;
    if (!options.callable_directory) { pending = static_cast<u32>(output.words.size()); Put(0xb4000010u); }
    count(abi::ContextHitsOffset);
    Put(0xd61f0200u); // This source site's BR x16.
    output.words[mismatch] |= (static_cast<u32>(output.words.size()) - mismatch) << 5;
    Put(0x39400000u | (abi::DispatchOccupiedOffset << 10) | (3u << 5) | 4u);
    const auto empty = static_cast<u32>(output.words.size()); Put(0x34000004u);
    Put(0x910004a5u); // ADD x5,x5,#1.
    Put(0x8a000000u | (6u << 16) | (5u << 5) | 5u);
    const auto distance = probe - static_cast<u32>(output.words.size());
    Put(0x14000000u | (distance & 0x03ffffffu));
    for (const auto edge : {no_slots, pending, empty})
        if (edge != UINT32_MAX) output.words[edge] |= (static_cast<u32>(output.words.size()) - edge) << 5;
    // A cold miss may repeat the lookup in the shared callback. Its actual
    // diagnostic probes are counted there too; only that owner resolves it.
    TailJump(options.dispatcher);
}

void Writer::PublishPCAndJump(u64 target_pc) {
    if (!options.deferred_pc) { Immediate(0, target_pc); PCMemory(false); }
    TailJump(options.dispatcher, true, target_pc);
}

void Writer::Epilogue() {
    StackAdjust(false);
    if (Linked()) TailJump(options.exit_handler);
    else { Put(0xa8c17bf3); Put(0xd65f03c0); }
}

unsigned Writer::PrepareTerminalCondition(unsigned condition, bool& restore, bool& flip) {
    if (condition == 16 || condition == 17) {
        SnapshotCompareNZCV();
        nzcv_matches = 0;
        if (RawAF()) {
            Put(0xca000000u | (1u << 16) | (29u << 5) | 16u);
            Put(0xf27c021fu); // TST x16,#16: canonical AF is the xor's bit4.
        } else Put(0xf266003fu); // TST x1,#(1<<26).
        condition = condition == 16 ? 1u : 0u;
        restore = NativeNZCV();
    } else if (NativeNZCV()) {
        if (condition == 20 || condition == 21) condition = 9u ^ (condition & 1u); // Guest C|Z -> native LS.
        else if (condition == 2 || condition == 3) condition ^= 1u;
        else if (condition == 8 || condition == 9) { Put(0xd500401fu); flip = true; }
    } else EnsureNZCV(condition);
    return condition;
}

void Writer::RestoreTerminalCondition(bool restore, bool flip) {
    if (flip) Put(0xd500401fu);
    else if (restore) RestoreCompareNZCV();
}

void Writer::ConditionPC(unsigned condition, u64 taken, u64 fallthrough) {
    if (condition == 14 || condition == 15 || taken == fallthrough) {
        PublishPCAndJump(taken);
        return;
    }
    bool restore{}, flip{};
    condition = PrepareTerminalCondition(condition, restore, flip);
    if (source.ScopeKind() == Scope::Block && options.deferred_pc && !restore && !flip) {
        // Backward/self yes edge takes priority; otherwise choose no. This is
        // only a static layout preference, not a guest fallthrough contract.
        const bool primary_yes = taken <= options.entry_pc;
        const auto primary = primary_yes ? taken : fallthrough;
        const auto other = primary_yes ? fallthrough : taken;
        const auto head = static_cast<u32>(output.words.size());
        Put(0x54000000u | (condition ^ (primary_yes ? 0u : 1u)));
        PublishPCAndJump(other);
        TailJump(options.dispatcher, true, primary, head);
        return;
    }
    const auto skip = static_cast<u32>(output.words.size()); Put(0x54000000u | (condition ^ 1u));
    RestoreTerminalCondition(restore, flip);
    PublishPCAndJump(taken);
    output.words[skip] |= (static_cast<u32>(output.words.size()) - skip) << 5;
    RestoreTerminalCondition(restore, flip);
    PublishPCAndJump(fallthrough);
}

void Writer::ConditionEdges(unsigned condition, const StoredTerminal& terminal, BlockId next) {
    if (condition == 14 || condition == 15 || terminal.first == terminal.second) {
        if (terminal.first != next) Branch(terminal.first);
        return;
    }
    bool restore{}, flip{};
    condition = PrepareTerminalCondition(condition, restore, flip);
    const bool true_falls_through = terminal.first == next;
    const auto skip = true_falls_through ? condition : condition ^ 1;
    if (restore || flip) {
        const auto at = static_cast<u32>(output.words.size()); Put(0x54000000u | skip);
        RestoreTerminalCondition(restore, flip);
        Branch(true_falls_through ? terminal.second : terminal.first);
        output.words[at] |= (static_cast<u32>(output.words.size()) - at) << 5;
        RestoreTerminalCondition(restore, flip);
    } else if (!ShortBranch(0x54000000u | (skip ^ 1u),
                           true_falls_through ? terminal.second : terminal.first)) {
        Put(0x54000040u | skip); // B.cond +8: only the actual long edge uses B26.
        Branch(true_falls_through ? terminal.second : terminal.first);
    }
    if (!true_falls_through && terminal.second != next) Branch(terminal.second);
}

void Writer::Terminal(const StoredTerminal& terminal, BlockId next) {
    CompleteLocalFlags();
    switch (terminal.kind) {
        case TerminalKind::Return:
            Shadow(false);
            if (terminal.HasValue()) {
                const auto value = RawValue(terminal.value.value, 16);
                const auto type = terminal.value_type == Type::Void ? source.Operations()[terminal.value.value].type : terminal.value_type;
                Narrow(0, value, Width(type));
            } else Immediate(0, 0);
            Epilogue(); break;
        case TerminalKind::Dispatch:
            if (!options.private_uniforms) Shadow(false);
            if (Linked()) {
                StackAdjust(false);
                if (static_pc_valid && alternate_pc_valid) {
                    // The selected PC is already committed. Use its two
                    // real constant edges without extending the dead SSA
                    // condition's lifetime or introducing a second cache.
                    SnapshotCompareNZCV();
                    PCMemory(true); Immediate(17, static_pc);
                    Put(0xeb00001fu | (17u << 16));
                    const auto skip = static_cast<u32>(output.words.size()); Put(0x54000001u);
                    RestoreCompareNZCV();
                    TailJump(options.dispatcher, true, static_pc);
                    output.words[skip] |= (static_cast<u32>(output.words.size()) - skip) << 5;
                    RestoreCompareNZCV();
                    TailJump(options.dispatcher, true, alternate_pc);
                } else {
                    if (!options.deferred_pc || !static_pc_valid) PCMemory(true);
                    if (static_pc_valid) TailJump(options.dispatcher, true, static_pc);
                    else DynamicDispatch();
                }
            }
            else { Immediate(0, 0); Epilogue(); }
            break;
        case TerminalKind::Jump: if (terminal.first != next) Branch(terminal.first); break;
        case TerminalKind::Branch: {
            const auto& home = allocation.homes[terminal.value.value];
            const auto type = terminal.value_type == Type::Void ? source.Operations()[terminal.value.value].type : terminal.value_type;
            if (deferred_test.bits) {
                Width(type);
                const auto& deferred = deferred_test;
                const auto cbnz = (deferred.bits == 64 ? 0xb5000000u : 0x35000000u) | deferred.value_register;
                if (!ShortBranch(cbnz, terminal.first)) {
                    Put((cbnz ^ 0x01000000u) | (2u << 5));
                    Branch(terminal.first);
                }
                // Only the zero exit completes TEST. Its current value,
                // not the nonzero entry snapshot, is known to be zero.
                Immediate(1, 0); Immediate(29, 0);
                Put(0xf100003fu); // CMP x1,#0: N0 Z1 !guestC1 V0.
                shadow_dirty = true; packed_current = false; nzcv_matches = 0;
                if (terminal.second != next) Branch(terminal.second);
                break;
            }
            if (home.terminal_only) {
                Width(type); // Preserve the terminal's scalar view contract.
                ConditionEdges(terminal_condition, terminal, next); break;
            }
            if (terminal.first == terminal.second) {
                if (terminal.first != next) Branch(terminal.first);
                break;
            }
            const auto value = RawValue(terminal.value.value, 16);
            Narrow(16, value, Width(type));
            const bool true_falls_through = terminal.first == next;
            if (!ShortBranch((true_falls_through ? 0xb4000000u : 0xb5000000u) | 16u,
                             true_falls_through ? terminal.second : terminal.first)) {
                Put(true_falls_through ? 0xb5000050u : 0xb4000050u); // CBNZ/CBZ x16,+8
                Branch(true_falls_through ? terminal.second : terminal.first);
            }
            if (!true_falls_through && terminal.second != next) Branch(terminal.second);
            break;
        }
        case TerminalKind::Unreachable: Shadow(false); Put(0xd4200000); break;
        default: Reject(CompileError::UnsupportedControlFlow);
    }
}

} // namespace detail

CompileResult Compile(const ir::Program& program, CompileOptions options) {
    auto allocation = program.ScopeKind() == ir::Scope::Block ? AllocateBlock(program, &options) :
            AllocateFunction(program, &options);
    CompileResult result;
    if (!allocation) {
        result.error = allocation.error; result.node = allocation.node; result.block = allocation.block; return result;
    }
    result.frame_bytes = (options.dispatcher || options.exit_handler ? 0 : 16) + ((allocation.spill_bytes + 15) & ~15u);
    result.spill_slots = allocation.spill_bytes / 8;
    result.words.reserve(program.Operations().size() * 2 + 32); // Hint only; emission still grows and checks actual size.
    try {
        if (!detail::Writer(program, allocation, result, options).Run()) {
            // No slice has been published. Keep the actual output capacity
            // and Allocation; fresh Writer state rebuilds every word-based
            // branch, fault and link location exactly once in the old form.
            result.words.clear(); result.faults.clear(); result.links.clear();
            (void)detail::Writer(program, allocation, result, options, true).Run();
        }
    }
    catch (const detail::Rejected& failure) {
        result.error = failure.error; result.node = failure.node; result.block = failure.block;
        result.words.clear(); result.faults.clear(); result.links.clear();
    }
    return result;
}

} // namespace swift::runtime::backend::arm64
