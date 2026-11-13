#include "emit.h"
#include "runtime/exit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {
void Writer::OrderedMemory(
        bool load, unsigned bits, unsigned value, unsigned address, std::int32_t offset) {
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) Reject(CompileError::UnsupportedType);
    const auto size = bits == 64 ? 3u : bits == 32 ? 2u : bits == 16 ? 1u : 0u;
    // Without an installed service and real PC marker, retain the actual
    // alignment guard. Recoverable accesses keep only the actual fast
    // instruction here; Run closes their cold half-barrier fallbacks.
    const bool recover =
            size && options.fault_locations && options.recover_unaligned && has_fault_location;
    if (size && !recover) {
        Extract(0, address, 0, size);
        __ cbnz(v::x0, 3);
    }
    const bool imm9 = RcpcImm9();
    const MemOperand memory{Base(address), imm9 ? offset : 0};
    const auto emit_ordered = [&] {
        const auto first = static_cast<u32>(output.words.size());
        if (imm9) {
            if (load) {
                if (bits == 8)
                    __ ldapurb(W(value), memory);
                else if (bits == 16)
                    __ ldapurh(W(value), memory);
                else
                    __ ldapur(R(value, bits), memory);
            } else {
                if (bits == 8)
                    __ stlurb(W(value), memory);
                else if (bits == 16)
                    __ stlurh(W(value), memory);
                else
                    __ stlur(R(value, bits), memory);
            }
        } else if (load && options.rcpc_loads) {
            if (bits == 8)
                __ ldaprb(W(value), memory);
            else if (bits == 16)
                __ ldaprh(W(value), memory);
            else
                __ ldapr(R(value, bits), memory);
        } else if (load) {
            if (bits == 8)
                __ ldarb(W(value), memory);
            else if (bits == 16)
                __ ldarh(W(value), memory);
            else
                __ ldar(R(value, bits), memory);
        } else {
            if (bits == 8)
                __ stlrb(W(value), memory);
            else if (bits == 16)
                __ stlrh(W(value), memory);
            else
                __ stlr(R(value, bits), memory);
        }
        RecordGuestAccess(first, load ? 1 : 2, bits / 8);
    };
    if (recover) {
        unaligned_word = static_cast<u32>(output.words.size());
        emit_ordered();
        return;
    }
    emit_ordered();
    if (!size) return;
    __ b(3);
    if (load) {
        GuestMemory(true, bits, value, address);
        __ dmb(v::InnerShareable, v::BarrierReads);
    } else {
        __ dmb(v::InnerShareable, v::BarrierAll);
        GuestMemory(false, bits, value, address);
    }
}

void Writer::HandleLoadMemoryFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    const auto& metadata = ctx.metadata;
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    const auto vector = ctx.vector;
    const auto test_prefix = ctx.test_prefix;
    auto& saved = ctx.saved;
    if (vector) {
        if (!options.private_uniforms) Shadow(false);
        const auto address = Expression(cursor, 16, 17);
        fault_parity = static_cast<std::uint8_t>(deferred_parity);
        fault_carry = direct_carry;
        GuestVectorMemory(true, destination, address);
        if (operation.opcode == OpCode::LoadMemoryTSO)
            __ dmb(v::InnerShareable, v::BarrierReads);  // DMB ISHLD.
        return;
    }
    if (test_prefix) {
        const auto address = *cursor.at;
        deferred_test.active =
                deferred_test.captured && !deferred_test.loaded && has_fault_location &&
                !deferred_test.site_count &&
                (operation.type == Type::U32 || operation.type == Type::U64) &&
                address.Kind() == OperandKind::Value &&
                address.Payload() == deferred_test.snapshot.value &&
                (address.TypeView() == Type::Void || address.TypeView() == Type::U64);
        if (deferred_test.active) deferred_test.loaded = node;
    }
    if (!options.private_uniforms)
        Shadow(false);  // Private ABI recovers x1 from the actual fault context.
    const bool ordered = operation.opcode == OpCode::LoadMemoryTSO;
    std::int32_t offset{};
    const auto address = ordered ? OrderedAddress(cursor, offset) : Expression(cursor, 16, 17);
    fault_parity = static_cast<std::uint8_t>(deferred_parity);
    fault_carry = direct_carry;
    if (ordered && (operation.type == Type::U16 || operation.type == Type::U32) &&
        metadata == OperationMetadata{} && RcpcImm9() && !home.spilled &&
        node.value + 1 < source.Operations().size()) {
        const NodeId consumer{node.value + 1};
        const auto& next = source.Operations()[consumer.value];
        const auto& next_home = allocation.homes[consumer.value];
        if (next.opcode == OpCode::SignExtend &&
            next.type == (width == 16 ? Type::U32 : Type::U64) && next.block == operation.block &&
            next.Metadata() == OperationMetadata{} && !next_home.spilled &&
            home.last_use == next_home.definition_position && next_home.reg != deferred_parity) {
            const auto inputs = source.Operands(consumer);
            auto at = inputs.begin();
            if (at != inputs.end()) {
                const auto input = *at++;
                if (at == inputs.end() && input.Kind() == OperandKind::Value &&
                    input.Payload() == node.value && Width(View(input)) == width) {
                    // The adjacent pure SXT is the load's only use. A fault
                    // leaves its destination (including a guest pin) unchanged.
                    unaligned_word = static_cast<u32>(output.words.size());
                    if (width == 16)
                        __ ldapursh(W(next_home.reg), MemOperand(Base(address), offset));
                    else
                        __ ldapursw(X(next_home.reg),
                                    MemOperand(Base(address),
                                               offset));  // LDAPURSH W / LDAPURSW X.
                    RecordGuestAccess(unaligned_word, 1, width / 8);
                    elided_consumer = consumer;
                    saved = true;
                    return;
                }
            }
        }
    }
    if (ordered)
        OrderedMemory(true, width, destination, address, offset);
    else
        GuestMemory(true, width, destination, address);
}

void Writer::HandleStoreMemoryFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto test_prefix = ctx.test_prefix;
    if (test_prefix) {
        const auto address = *cursor.at;
        deferred_test.active =
                deferred_test.value && has_fault_location && deferred_test.site_count == 1 &&
                address.Kind() == OperandKind::Value &&
                address.Payload() == deferred_test.snapshot.value &&
                (address.TypeView() == Type::Void || address.TypeView() == Type::U64);
    }
    if (!options.private_uniforms) Shadow(false);
    const bool ordered = operation.opcode == OpCode::StoreMemoryTSO;
    std::int32_t offset{};
    const auto address =
            ordered ? OrderedAddress(cursor, offset, true) : Expression(cursor, 16, 17);
    const auto input = cursor.Take();
    if (deferred_test.active) {
        const auto type = input.Kind() == OperandKind::Value
                                  ? source.Operations()[input.Payload()].type
                                  : Type::Void;
        deferred_test.active = IntegerType(type) && TypeBits(type) <= 64;
    }
    const bool vector_input = input.Kind() == OperandKind::Value &&
                              source.Operations()[input.Payload()].type == Type::V128;
    const auto bits = vector_input ? 128u : Width(View(input));
    const auto value =
            vector_input ? VectorValue(input, 0) : Read(input, 17, 64, false, bits == 32 ? 32 : 64);
    if (vector_input) {
        CompleteLocalFlags();
        if (operation.opcode == OpCode::StoreMemoryTSO) __ dmb(v::InnerShareable, v::BarrierAll);
        GuestVectorMemory(false, value, address, static_cast<u32>(offset) / 16);
    } else {
        fault_parity = static_cast<std::uint8_t>(deferred_parity);
        fault_carry = direct_carry;
        if (ordered)
            OrderedMemory(false, bits, value, address, offset);
        else
            GuestMemory(false, bits, value, address);
    }
    if (!options.private_uniforms) Shadow(true);  // Generic raw pointers may alias the flags field.
}

void Writer::HandleCheckMemoryAlignmentFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto address_cell = cursor.Take();
    const auto bits = IntegerImmediate(cursor.Take());
    if (address_cell.Kind() == OperandKind::Immediate && (address_cell.Payload() & bits) == 0)
        return;
    if (address_cell.Kind() == OperandKind::Value &&
        address_cell.Payload() < source.Operations().size()) {
        const auto id = static_cast<u32>(address_cell.Payload());
        const auto& def = source.Operations()[id];
        if (def.opcode == OpCode::Zero) return;
        if (def.opcode == OpCode::LoadImm) {
            const auto args = source.Operands(NodeId{id});
            if (args.begin() != args.end() && args.front().Kind() == OperandKind::Immediate &&
                (args.front().Payload() & bits) == 0)
                return;
        }
    }
    if (!options.private_uniforms) Shadow(false);
    const auto address = Read(address_cell, 16);
    LogicalMask mask;
    if (mask.Encode(bits, 64))
        mask.Emit(masm.exact, v::x0, X(address), v::AND);
    else {
        Immediate(17, bits);
        __ and_(v::x0, X(address), Operand(v::x17));
    }
    if (!long_forward_branches && cold_exit_count < cold_exit_stubs.size()) {
        cold_exit_stubs[cold_exit_count++] = {masm.Here(),
                                              fault_location,
                                              ExitValue(NativeExit::AlignmentFault),
                                              nzcv_matches,
                                              0,
                                              0,
                                              0,
                                              has_fault_location && options.pc_offset != UINT32_MAX,
                                              shadow_dirty,
                                              packed_current,
                                              false};
        __ cbnz(v::x0, std::int64_t{0});
        return;
    }
    const auto aligned = masm.Here();
    __ cbz(v::x0, std::int64_t{0});
    const auto dirty = shadow_dirty, packed = packed_current;
    const auto matching = nzcv_matches;
    Shadow(false);
    if (has_fault_location && options.pc_offset != UINT32_MAX) {
        Immediate(0, fault_location);
        PCMemory(false);
    }
    Immediate(0, ExitValue(NativeExit::AlignmentFault));
    Epilogue();
    shadow_dirty = dirty;
    packed_current = packed;
    nzcv_matches = matching;
    masm.Patch(aligned, masm.Here());
}

void Writer::HandleMemoryBarrierTSOFamily(OpContext& /*ctx*/) {
    __ dmb(v::InnerShareable, v::BarrierAll);
}

void Writer::HandleMemoryStoreFenceFamily(OpContext& /*ctx*/) {
    __ dmb(v::FullSystem, v::BarrierWrites);  // DMB ST.
}

void Writer::HandleMemoryLoadFenceFamily(OpContext& ctx) {
    if (ctx.operation.opcode == OpCode::CpuYield)
        __ hint(v::YIELD);
    else
        __ dmb(v::InnerShareable, v::BarrierReads);
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
