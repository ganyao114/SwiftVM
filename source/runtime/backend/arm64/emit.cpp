#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {
bool Writer::Run() try {
    if (NativeNZCV()) RequireFlags();
    if (Enabled() && options.flags_offset > UINT32_MAX - 7) Reject(CompileError::UnsupportedFlags);
    if (Linked() &&
        (!options.dispatcher || !options.exit_handler || options.pc_offset > UINT32_MAX - 7))
        Reject(CompileError::UnsupportedOperand);
    if (options.dispatch_mode != DispatchMode::Callback && !Linked())
        Reject(CompileError::UnsupportedOperand);
    if (Linked() && Enabled() && options.flags_offset < u64{options.pc_offset} + 8 &&
        options.pc_offset < u64{options.flags_offset} + 8)
        Reject(CompileError::UnsupportedOperand);
    if (options.private_uniforms && (!Linked() || !Pinned()))
        Reject(CompileError::UnsupportedOperand);
    if (VectorPinned() && (!Linked() || !options.private_uniforms || !options.fault_locations ||
                           options.pinned_vector_offset > UINT32_MAX - 255u))
        Reject(CompileError::UnsupportedOperand);
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
        if (options.pinned_uniform_offset > UINT32_MAX - (bytes - 1))
            Reject(CompileError::UnsupportedOperand);
        const auto overlaps = [&](u32 field) {
            return field != UINT32_MAX && u64{field} < u64{options.pinned_uniform_offset} + bytes &&
                   options.pinned_uniform_offset < u64{field} + 8;
        };
        if (overlaps(options.pc_offset) || overlaps(options.flags_offset))
            Reject(CompileError::UnsupportedOperand);
    }
    if (!Linked()) {
        masm.exact.stp(
                v::x19, v::x30, MemOperand(v::sp, -16, v::PreIndex));  // STP x19,x30,[sp,#-16]!
        Move(19, 0);
    }
    StackAdjust(true);
    if (!Linked()) Shadow(true);
    if (allocation.publish_entry_pc) {
        Immediate(0, options.entry_pc);
        PCMemory(false);
    }
    // Consume allocation's one CFG schedule; source IDs/storage are not
    // rewritten. Entry's reachable component is first, then unreachable.
    for (std::size_t order = 0; order < allocation.block_order.size(); ++order) {
        const auto b = allocation.block_order[order];
        block = BlockId{b};
        masm.labels[b] = static_cast<u32>(output.words.size());
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
        deferred_logical = {};
        deferred_parity = 0;
        direct_carry = false;
        pending_denormal.check_mask = 0;
        pending_denormal.protected_mask = 0;
        pending_denormal.op_count = 0;
        checked_alignment_count = 0;
        v128_anchor_words = UINT32_MAX;
        v128_anchor_pending = false;
        tso_store_barrier_ready = false;
        vec_mul_widen_words = UINT32_MAX;
        const auto& body = source.Blocks()[b];
        deferred_test = {};
        deferred_test.active =
                !deferred_test_used && RawAF() && Pinned() && options.private_uniforms &&
                options.fault_locations && options.recover_unaligned &&
                ((source.ScopeKind() == Scope::Function && body.node_count >= 3 &&
                  body.terminal.kind == TerminalKind::Branch && body.terminal.first == block &&
                  body.terminal.second != block &&
                  body.terminal.value.value == body.first_node + body.node_count - 1) ||
                 (source.ScopeKind() == Scope::Block && !output.frame_bytes &&
                  !allocation.publish_entry_pc && body.node_count >= 4 &&
                  body.terminal.kind == TerminalKind::Dispatch));
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            node = NodeId{i};
            const auto& operation = source.Operations()[i];
            if (operation.opcode == OpCode::EntryPoint) {
                if (source.ScopeKind() != Scope::Function || !Linked())
                    Reject(CompileError::UnsupportedControlFlow);
                if (i != body.first_node || operation.Metadata() != OperationMetadata{})
                    Reject(CompileError::InvalidProgram);
                const auto inputs = source.Operands(node);
                Cursor cursor{inputs.begin(), inputs.end(), *this};
                const auto pc = cursor.Take();
                if (pc.TypeView() != Type::U64) Reject(CompileError::UnsupportedOperand);
                output.entries.push_back({IntegerImmediate(pc), masm.labels[b] * 4});
                cursor.Done();
                continue;  // A declaration must not disturb the local TEST prefix.
            }
            if ((operation.opcode == OpCode::Nop || operation.opcode == OpCode::FaultLocation) &&
                operation.Metadata() == OperationMetadata{}) {
                if (operation.opcode == OpCode::Nop) continue;
                if (!options.fault_locations) Reject(CompileError::UnsupportedOpcode);
                fault_location = IntegerImmediate(source.Operands(node).front());
                has_fault_location = true;
                continue;
            }
            unaligned_word = UINT32_MAX;
            fault_parity = fault_carry = fault_logical_code = 0;
            const auto effects = EffectsFor(operation.opcode, operation.Metadata());
            Emit(operation, effects);
        }
        if (pending_save) Reject(CompileError::UnsupportedFlags);
        if (!masm.local_fixups.empty()) Reject(CompileError::InvalidProgram);
        const auto next = order + 1 < allocation.block_order.size()
                                  ? BlockId{allocation.block_order[order + 1]}
                                  : BlockId{};
        node = {};
        if (!dispatch_emitted) Terminal(body.terminal, next);
    }
    for (const auto& fixup : masm.fixups) {
        const auto distance = static_cast<std::int64_t>(masm.labels[fixup.target]) - fixup.word;
        const auto* instruction = v::Instruction::Cast(output.words.data() + fixup.word);
        if (instruction->IsUncondBranchImm()) {
            if (distance < -(std::int64_t{1} << 25) || distance >= (std::int64_t{1} << 25))
                Reject(CompileError::CodeTooLarge);
        } else {
            if (distance < -(std::int64_t{1} << 18) || distance >= (std::int64_t{1} << 18)) {
                if (!long_forward_branches) return false;
                Reject(CompileError::CodeTooLarge);
            }
        }
        masm.Patch(fixup.word, masm.labels[fixup.target]);
    }
    // Keep entry prologues outside the body schedule: even an internal
    // fallthrough must skip the frame allocation. Reuse the actual output
    // resources to turn their temporary body words into callable stub words.
    for (auto& entry : output.entries) {
        const auto body_word = entry.offset / 4;
        entry.offset = static_cast<u32>(output.words.size()) * 4;
        StackAdjust(true);
        if (allocation.publish_entry_pc) {
            Immediate(0, entry.pc);
            PCMemory(false);
        }
        const auto distance = static_cast<std::int64_t>(body_word) -
                              static_cast<std::int64_t>(output.words.size());
        if (distance < -(INT64_C(1) << 25) || distance >= (INT64_C(1) << 25))
            Reject(CompileError::CodeTooLarge);
        masm.exact.b(distance);
    }
    // Close only the actual recoverable sites, after every hot block and link
    // stub. A SIGBUS patches the one fast instruction to this cold sequence.
    for (auto& site : output.faults) {
        if (site.unaligned_offset == UINT32_MAX) continue;
        const auto fallback = output.words.size();
        const auto outbound = static_cast<std::int64_t>(fallback) - (site.unaligned_offset / 4);
        const auto inbound = static_cast<std::int64_t>((site.unaligned_offset / 4)) + 1 -
                             (static_cast<std::int64_t>(fallback) + 2);
        if (outbound < -(std::int64_t{1} << 25) || outbound >= (std::int64_t{1} << 25) ||
            inbound < -(std::int64_t{1} << 25) || inbound >= (std::int64_t{1} << 25))
            Reject(CompileError::CodeTooLarge);
        const auto fast = output.words[(site.unaligned_offset / 4)];
        const bool load = OrderedAccess(fast) == OrderedKind::Load;
        site.fallback_offset = static_cast<u32>(fallback) * 4;
        if (load) {
            PlainOrderedAccess(masm.exact, fast);
            masm.exact.dmb(v::InnerShareable, v::BarrierReads);
        } else {
            masm.exact.dmb(v::InnerShareable, v::BarrierAll);
            PlainOrderedAccess(masm.exact, fast);
        }
        masm.exact.b(inbound);
    }
    for (u32 i = 0; i < cold_denormal_count; ++i) {
        const auto& stub = cold_denormal_stubs[i];
        const auto target = masm.Here();
        for (unsigned b = 0; b < stub.branch_count; ++b) {
            const auto distance = static_cast<std::int64_t>(target) -
                                  static_cast<std::int64_t>(stub.branch_words[b]);
            if (distance < -(INT64_C(1) << 18) || distance >= (INT64_C(1) << 18)) {
                if (!long_forward_branches) return false;
                Reject(CompileError::CodeTooLarge);
            }
            masm.Patch(stub.branch_words[b], target);
        }
        const auto disabled = DenormalControlGuard();
        if (stub.batch_op_count) {
            for (unsigned k = 0; k < stub.batch_op_count; ++k) {
                const auto& op = stub.batch_ops[k];
                const u32 denom = op.denominator == UINT8_MAX ? UINT32_MAX : u32{op.denominator};
                RecordDenormalLanes(std::span<const std::uint8_t>{op.inputs.data(), op.input_count},
                                    stub.bits,
                                    stub.count,
                                    std::span<const std::uint8_t>{op.peers.data(), op.peer_count},
                                    denom,
                                    UINT32_MAX,
                                    false);
            }
        } else {
            RecordDenormalLanes(std::span<const std::uint8_t>{stub.inputs.data(), stub.input_count},
                                stub.bits,
                                stub.count,
                                std::span<const std::uint8_t>{stub.peers.data(), stub.peer_count},
                                stub.denominator,
                                stub.fma_flags,
                                stub.negative_invalid);
        }
        if (disabled != UINT32_MAX) masm.Patch(disabled, masm.Here());
        const auto back = static_cast<std::int64_t>(stub.return_word) -
                          static_cast<std::int64_t>(masm.Here());
        if (back < -(INT64_C(1) << 25) || back >= (INT64_C(1) << 25))
            Reject(CompileError::CodeTooLarge);
        masm.exact.b(back);
    }
    for (u32 i = 0; i < cold_exit_count; ++i) {
        const auto& stub = cold_exit_stubs[i];
        const auto target = masm.Here();
        const auto distance =
                static_cast<std::int64_t>(target) - static_cast<std::int64_t>(stub.branch_word);
        if (distance < -(INT64_C(1) << 18) || distance >= (INT64_C(1) << 18)) {
            if (!long_forward_branches) return false;
            Reject(CompileError::CodeTooLarge);
        }
        masm.Patch(stub.branch_word, target);
        shadow_dirty = stub.dirty;
        packed_current = stub.packed;
        nzcv_matches = stub.matching;
        deferred_parity = stub.parity_source;
        direct_carry = stub.carry_source;
        deferred_logical = {stub.logical_reg, stub.logical_bits};
        deferred_test.bits = stub.deferred_bits;
        deferred_test.value_register = stub.deferred_value_reg;
        if (deferred_test.bits) CompleteDeferredTest();
        Shadow(false);
        if (stub.has_pc) {
            Immediate(0, stub.pc);
            PCMemory(false);
        }
        Immediate(0, stub.exit_value);
        Epilogue();
    }
    return true;
} catch (const vixl::CodeBufferOverflow&) {
    Reject(CompileError::CodeTooLarge);
}

void Writer::Emit(const Operation& operation, Effect effects) {
    const auto metadata = operation.Metadata();
    const bool test_prefix = deferred_test.active &&
                             (metadata == OperationMetadata{} || operation.opcode == OpCode::And);
    // Only the actual supported prefix cases below keep this candidate
    // alive. All other operations follow normal emission unchanged.
    deferred_test.active = false;
    if (static_pc_valid &&
        HasAnyEffect(effects, Effect::WriteMemory | Effect::Unknown | Effect::External))
        static_pc_valid = false;
    if (static_pc_valid && operation.opcode != OpCode::StoreUniform &&
        HasAnyEffect(effects, Effect::WriteGuestState))
        static_pc_valid = false;
    Metadata(operation);
    const auto legacy_mask = CaptureLegacy(operation);
    const auto flag_mask = metadata.flag_outputs | legacy_mask;
    if (flag_mask || HasAnyEffect(effects, Effect::WriteFlags)) shadow_dirty = true;
    const auto cells = source.Operands(node);
    Cursor cursor{cells.begin(), cells.end(), *this};
    const bool produces = operation.type != Type::Void;
    const auto& home = allocation.homes[node.value];
    const bool vector = operation.type == Type::V128;
    const unsigned destination = produces && !home.spilled ? home.reg
                                 : vector                  ? VectorTemporary(true)
                                                           : 16;
    const auto width = produces && !vector ? Width(operation.type) : 0;
    if (pending_denormal.op_count) {
        const bool batch_candidate =
                operation.opcode == OpCode::VecFAdd || operation.opcode == OpCode::VecFSub ||
                operation.opcode == OpCode::VecFMul || operation.opcode == OpCode::VecFDiv ||
                operation.opcode == OpCode::VecFCmpMask || operation.opcode == OpCode::VecFMinMax;
        bool flush_batch = !batch_candidate &&
                           HasAnyEffect(effects,
                                        Effect::MayFault | Effect::Unknown | Effect::External |
                                                Effect::Control | Effect::ReadHostState |
                                                Effect::WriteHostState);
        if (!flush_batch && !batch_candidate && produces && vector &&
            ((pending_denormal.protected_mask >> destination) & 1u)) {
            flush_batch = true;
            if (operation.opcode == OpCode::LoadUniform) {
                const auto uniform = *cells.begin();
                if (uniform.Kind() == OperandKind::Uniform && uniform.Payload() <= UINT32_MAX &&
                    VectorPinnedSlot(static_cast<u32>(uniform.Payload()), 16) ==
                            static_cast<int>(destination))
                    flush_batch = false;
            }
        }
        if (!flush_batch &&
            (operation.opcode == OpCode::LoadUniform || operation.opcode == OpCode::StoreUniform ||
             operation.opcode == OpCode::StoreUniformSelectImm)) {
            auto it = cells.begin();
            const auto uniform = *it++;
            if (options.fpsr_offset != UINT32_MAX && uniform.Payload() == options.fpsr_offset)
                flush_batch = true;
            else if (operation.opcode != OpCode::LoadUniform && VectorPinned() &&
                     uniform.Payload() <= UINT32_MAX) {
                const auto offset = static_cast<u32>(uniform.Payload());
                const auto bytes = TypeBits(uniform.TypeView()) / 8;
                if (offset < u64{options.pinned_vector_offset} + 256 &&
                    options.pinned_vector_offset < u64{offset} + bytes) {
                    const int pinned =
                            (operation.opcode == OpCode::StoreUniform &&
                             uniform.TypeView() == Type::V128)
                                    ? VectorPinnedSlot(offset, 16)
                                    : -1;
                    if (pinned < 0)
                        flush_batch = true;
                    else if ((pending_denormal.protected_mask >> pinned) & 1u) {
                        const auto val = *it;
                        const bool self = val.Kind() == OperandKind::Value &&
                                          val.Payload() < allocation.homes.size() &&
                                          !allocation.homes[val.Payload()].spilled &&
                                          allocation.homes[val.Payload()].reg ==
                                                  static_cast<unsigned>(pinned);
                        if (!self) flush_batch = true;
                    }
                }
            }
        }
        if (flush_batch) FlushDenormalBatch();
    }
    if (tso_store_barrier_ready &&
        (HasAnyEffect(effects, Effect::ReadMemory | Effect::Unknown | Effect::External) ||
         (HasAnyEffect(effects, Effect::Control) &&
          operation.opcode != OpCode::CheckMemoryAlignment &&
          operation.opcode != OpCode::GuestSafepoint &&
          operation.opcode != OpCode::GuestDispatchSafepoint)))
        tso_store_barrier_ready = false;
    // Full literal writers can replace the old local representation. All
    // actual readers and opaque boundaries retain the ordinary complete ABI.
    const bool reads_flags = HasAnyEffect(effects, Effect::ReadFlags);
    const auto pf_af = parity | auxiliary;
    const bool full_or_discard_flags =
            !reads_flags && !legacy_mask && metadata.flag_outputs &&
            ((metadata.flag_outputs & all_flags) == all_flags || metadata.discard_omitted_flags);
    if (full_or_discard_flags) {
        deferred_logical = {};
        deferred_parity = 0;
        direct_carry = false;
    } else if (deferred_logical.bits && !reads_flags && metadata.flag_outputs) {
        CompleteDeferredLogical();
    }
    const bool replaces_pf = !reads_flags && (metadata.flag_outputs & pf_af) == pf_af;
    if (replaces_pf) deferred_parity = 0;
    if (!reads_flags && (metadata.flag_outputs & carry)) direct_carry = false;
    const bool memory =
            operation.opcode == OpCode::LoadMemory || operation.opcode == OpCode::LoadMemoryTSO ||
            operation.opcode == OpCode::StoreMemory || operation.opcode == OpCode::StoreMemoryTSO;
    const bool alignment_guard =
            operation.opcode == OpCode::CheckMemoryAlignment && options.private_uniforms;
    if (operation.opcode != OpCode::GuestSafepoint &&
        operation.opcode != OpCode::GuestDispatchSafepoint && !alignment_guard &&
        (reads_flags || legacy_mask ||
         HasAnyEffect(effects, Effect::Unknown | Effect::External | Effect::Control) ||
         (metadata == OperationMetadata{} && HasAnyEffect(effects, Effect::WriteFlags)) ||
         (HasAnyEffect(effects, Effect::MayFault) && (!has_fault_location || !memory)) ||
         operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid ||
         operation.opcode == OpCode::ReadSystem))
        CompleteLocalFlags();
    else if ((metadata.flag_outputs & pf_af) && !replaces_pf)
        CompleteParity();
    // Borrowed/self Uniform loads are handled at their actual read/write
    // encoding point, so a no-op state read does not force a PF copy.
    if (deferred_parity && produces && !vector && operation.opcode != OpCode::LoadUniform &&
        destination == deferred_parity)
        CompleteParity();
    bool clear_logical_after_load = false;
    if (deferred_logical.bits && produces && !vector && operation.opcode != OpCode::LoadUniform &&
        destination == deferred_logical.reg) {
        const bool scalar_load =
                operation.opcode == OpCode::LoadMemory || operation.opcode == OpCode::LoadMemoryTSO;
        if (scalar_load && has_fault_location && FlagsDeadFrom(node.value + 1))
            clear_logical_after_load = true;
        else if (!scalar_load && FlagsDeadFrom(node.value))
            deferred_logical = {};
        else
            CompleteDeferredLogical();
    }

    OpContext ctx{operation,
                  cursor,
                  home,
                  metadata,
                  legacy_mask,
                  flag_mask,
                  destination,
                  width,
                  produces,
                  vector,
                  test_prefix};
    Dispatch(ctx);
    if (clear_logical_after_load) deferred_logical = {};
    ctx.cursor.Done();
    if (!ctx.saved) SaveResult(ctx);
}

void Writer::Dispatch(OpContext& ctx) {
    switch (ctx.operation.opcode) {
#define INST(name, ...)                                                                            \
    case OpCode::name:                                                                             \
        return Handle(Op<OpCode::name>{}, ctx);
#include "runtime/ir/opcodes.inc"

#undef INST
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
