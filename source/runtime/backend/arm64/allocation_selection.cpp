#include "allocation_detail.h"

#include <array>
#include <bit>

namespace swift::runtime::backend::arm64::detail {
// These helpers remain in the one reverse node/use walk. Inlining preserves
// that hot walk while naming its ordered PC, flags and physical-pin decisions.
[[gnu::always_inline]] inline void Allocator::CancelPending(BlockSelection& selection,
                                                            unsigned slot) {
    const auto bit = 1u << slot;
    if (!(selection.pending_mask & bit)) return;
    result.homes[selection.pending[slot]].reg = 0;
    selection.pending_mask &= ~bit;
}

[[gnu::always_inline]] inline unsigned Allocator::DetachPending(BlockSelection& selection,
                                                                u32 producer) {
    const auto reg = result.homes[producer].reg;
    if (!selection.pending_mask || !reg) return 32u;
    const auto slot = operations[producer].type == Type::V128
                              ? 16u + abi::PinnedVectorSlot(reg)
                              : abi::PinnedUniformSlot(reg, options->pinned_uniform_slots);
    const auto bit = 1u << slot;
    if (!(selection.pending_mask & bit) || selection.pending[slot] != producer) return 32u;
    selection.pending_mask &= ~bit;
    return slot;
}

[[gnu::always_inline]] inline void Allocator::UniformAccess(BlockSelection& selection,
                                                            CellView uniform,
                                                            u32 at,
                                                            bool write) {
    if (!write && !selection.pending_mask) return;
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
            CancelPending(selection, slot);
            if (write) selection.next_pinned_write[slot] = at;
        }
    }
}

[[gnu::always_inline]] inline bool Allocator::BeforeWrite(const BlockSelection& selection,
                                                          const Home& home,
                                                          u32 next,
                                                          unsigned pin,
                                                          bool vector) const {
    const auto& body = blocks[selection.block];
    if (home.last_use >= selection.end) return false;
    if (home.last_use < next) return true;
    if (home.last_use != next) return false;
    const auto writer = body.first_node + next - BlockStart(selection.block);
    return result.homes[writer].reg == pin && (operations[writer].type == Type::V128) == vector &&
           ReadsBeforeWrite(operations[writer].opcode, vector);
}

[[gnu::always_inline]] inline NodeId Allocator::ObservePC(u32 i,
                                                          CellRange inputs,
                                                          Effect effects,
                                                          const BlockSelection& selection) {
    const auto& operation = operations[i];
    const auto& body = blocks[selection.block];
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
        } else if (operation.opcode != OpCode::GuestSafepoint &&
                   HasAnyEffect(effects, Effect::ReadGuestState | Effect::WriteGuestState))
            // A safepoint observes registers and flags, but takes its PC from
            // its immediate on the cold exit. It never reads the state PC.
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
                producer.Metadata() == OperationMetadata{} &&
                producer.block.value == selection.block)
                pc_literal = NodeId{static_cast<u32>(value.Payload())};
        }
    }
    return pc_literal;
}

[[gnu::always_inline]] inline bool Allocator::RecordInputUses(u32 i,
                                                              CellRange inputs,
                                                              NodeId pc_literal,
                                                              const BlockSelection& selection) {
    const auto at = result.homes[i].definition_position;
    const auto& body = blocks[selection.block];
    for (const auto cell : inputs)
        if (cell.Kind() == OperandKind::Value) {
            const bool sole_pc_literal = pc_literal && cell.Payload() == pc_literal.value &&
                                         result.homes[cell.Payload()].last_use ==
                                                 result.homes[cell.Payload()].definition_position;
            if (cell.Payload() >= NodeId::invalid ||
                !RecordUse(static_cast<u32>(cell.Payload()), at, selection.block)) {
                Reject(CompileError::InvalidProgram, NodeId{i}, BlockId{selection.block});
                return false;
            }
            // Keep the real lifetime use. Only the final PC consumer
            // may consume this predicate/literal without a scalar home.
            if (sole_pc_literal)
                result.homes[cell.Payload()].terminal_only = true;
            else if (!(selection.pc_check.value == cell.Payload() &&
                       i + 1 == body.first_node + body.node_count))
                result.homes[cell.Payload()].terminal_only = false;
        }
    return true;
}

[[gnu::always_inline]] inline void Allocator::ObserveNZCV(u32 i,
                                                          CellRange inputs,
                                                          Effect effects,
                                                          BlockSelection& selection) {
    auto& home = result.homes[i];
    const auto& operation = operations[i];
    const auto metadata = operation.Metadata();
    if (raw_flags) {
        // Literal metadata already describes definite writes. Keep
        // the unclassified writers/real observers conservative,
        // without introducing another flags opcode classification.
        // This IR permission makes omitted flags' post-state
        // unspecified; it is not a cached liveness conclusion.
        const auto discarded =
                metadata.discard_omitted_flags ? FlagBits(Flag::NZCV) & ~metadata.flag_outputs : 0;
        home.nzcv_clobber = static_cast<std::uint8_t>(
                (FlagBits(Flag::NZCV) & ~selection.needed_nzcv) | discarded);
        selection.needed_nzcv &= ~metadata.flag_outputs;
        bool observed =
                HasAnyEffect(effects,
                             Effect::ReadFlags | Effect::MayFault | Effect::Unknown |
                                     Effect::External | Effect::Control) ||
                (metadata == OperationMetadata{} && HasAnyEffect(effects, Effect::WriteFlags));
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
        if (observed) selection.needed_nzcv = FlagBits(Flag::NZCV);
    }
}

[[gnu::always_inline]] inline void Allocator::SelectPinnedHome(u32 i,
                                                               CellRange inputs,
                                                               Effect effects,
                                                               BlockSelection& selection) {
    auto& home = result.homes[i];
    const auto& operation = operations[i];
    const auto at = home.definition_position;
    const auto& body = blocks[selection.block];
    if (selection.pending_mask) {
        // Only now is an unobserved delayed publication an actual
        // producer write. Earlier borrow origins have not been read.
        const auto confirmed = DetachPending(selection, i);
        if (confirmed != 32) selection.next_pinned_write[confirmed] = at;
    }
    if (selection.pending_mask) {
        const bool observer = HasAnyEffect(effects,
                                           Effect::MayFault | Effect::Unknown | Effect::External |
                                                   Effect::Control) ||
                              operation.opcode == OpCode::UniformBarrier ||
                              operation.opcode == OpCode::XchgBarrier ||
                              (HasAnyEffect(effects, Effect::ReadGuestState) &&
                               operation.opcode != OpCode::LoadUniform) ||
                              (HasAnyEffect(effects, Effect::WriteGuestState) &&
                               operation.opcode != OpCode::StoreUniform &&
                               operation.opcode != OpCode::StoreUniformSelectImm);
        if (observer) {
            while (selection.pending_mask)
                CancelPending(selection, std::countr_zero(selection.pending_mask));
        } else if (operation.opcode == OpCode::LoadUniform)
            UniformAccess(selection, inputs.front(), at, false);
    }
    if ((pinned || vector_pinned) && operation.opcode == OpCode::StoreUniform) {
        auto input = inputs.begin();
        const auto uniform = *input++, value = *input;
        const auto slot = UniformSlot(uniform);
        const bool full_slot = slot != 32 && value.Kind() == OperandKind::Value &&
                               uniform.TypeView() == (slot < 16 ? Type::U64 : Type::V128);
        const bool vector = slot >= 16;
        const auto pin = full_slot ? PinRegister(slot) : 0u;
        const auto later_write =
                full_slot && borrow_pins ? selection.next_pinned_write[slot] : selection.end;
        // A provisional home never changes the borrow cursor. Reads
        // can cancel it without repairing any earlier borrow decision.
        if (borrow_pins) UniformAccess(selection, uniform, at, true);
        if (full_slot) {
            const auto definition = static_cast<u32>(value.Payload());
            const auto& producer = operations[definition];
            auto& produced = result.homes[definition];
            const auto view = value.TypeView() == Type::Void ? producer.type : value.TypeView();
            const bool full_value =
                    vector ? producer.type == Type::V128 && view == Type::V128
                           : (producer.type == Type::U8 || producer.type == Type::U16 ||
                              producer.type == Type::U32 || producer.type == Type::U64) &&
                                     producer.opcode != OpCode::BitCast &&
                                     producer.opcode != OpCode::GetResult &&
                                     producer.opcode != OpCode::GetFlags && IntegerType(view) &&
                                     TypeBits(view) >= TypeBits(producer.type);
            if (full_value && !producer.Metadata().branch_only) {
                // One publication window, including the zero-length
                // window after an adjacent producer. Later uses must
                // finish before B ends and no later than a proven
                // read-before-write replacement of this actual pin.
                const bool immediate = i > body.first_node && definition == i - 1;
                const bool lifetime =
                        produced.last_use == at ||
                        (borrow_pins && BeforeWrite(selection, produced, later_write, pin, vector));
                if (lifetime &&
                    (immediate ||
                     (borrow_pins && !produced.reg && producer.block.value == selection.block &&
                      !HasAnyEffect(EffectsFor(producer.opcode, producer.Metadata()),
                                    Effect::Unknown | Effect::External | Effect::Control)))) {
                    if (selection.pending_mask) (void)DetachPending(selection, definition);
                    produced.reg = static_cast<std::uint8_t>(pin);
                    produced.narrow_pin = 0;  // Full publication must really normalize the carrier.
                    if (immediate) {
                        if (borrow_pins)
                            selection.next_pinned_write[slot] = produced.definition_position;
                    } else {
                        // Do not hoist the actual write until the
                        // producer is reached without an observer.
                        selection.pending[slot] = definition;
                        selection.pending_mask |= 1u << slot;
                    }
                }
            }
        }
    } else if (borrow_pins) {
        if (operation.opcode == OpCode::StoreUniformSelectImm)
            UniformAccess(selection, inputs.front(), at, true);
        else if (HasAnyEffect(effects,
                              Effect::Unknown | Effect::External | Effect::WriteGuestState))
            selection.next_pinned_write.fill(at);
    }
    if (borrow_pins && !home.reg && operation.opcode == OpCode::LoadUniform &&
        home.last_use < selection.end) {
        const auto uniform = inputs.front();
        const auto slot = UniformSlot(uniform);
        const bool vector = operation.type == Type::V128;
        const bool supported = vector || operation.type == Type::U8 ||
                               operation.type == Type::U16 || operation.type == Type::U32 ||
                               operation.type == Type::U64;
        if (slot != 32 && (slot >= 16) == vector && supported &&
            uniform.TypeView() == operation.type) {
            const auto pin = PinRegister(slot);
            if (BeforeWrite(selection, home, selection.next_pinned_write[slot], pin, vector)) {
                home.reg = static_cast<std::uint8_t>(pin);
                home.narrow_pin = operation.type == Type::U8    ? 1
                                  : operation.type == Type::U16 ? 2
                                  : operation.type == Type::U32 ? 3
                                                                : 0;
            }
        }
    }
}

bool Allocator::SelectHomes() {
    for (auto order = result.block_order.size(); order-- > 0;) {
        const auto b = result.block_order[order];
        const auto& body = blocks[b];
        BlockSelection selection{b, BlockStart(b) + body.node_count, TailPCCondition(b)};
        if (body.terminal.HasValue() && !RecordUse(body.terminal.value.value, selection.end)) {
            Reject(CompileError::InvalidProgram, {}, BlockId{b});
            return false;
        }
        if (body.terminal.HasValue() && operations[body.terminal.value.value].block.value != b)
            result.homes[body.terminal.value.value].terminal_only = false;
        if (borrow_pins) selection.next_pinned_write.fill(selection.end);
        for (auto i = body.first_node + body.node_count; i-- > body.first_node;) {
            const auto& operation = operations[i];
            const auto inputs = program.Operands(NodeId{i});
            const auto effects = borrow_pins || raw_flags || deferred_pc
                                         ? EffectsFor(operation.opcode, operation.Metadata())
                                         : Effect::None;
            const auto pc_literal = ObservePC(i, inputs, effects, selection);
            // Their admitted schemas contain no Value edges. They still
            // participate in PC, flags and delayed physical-pin decisions.
            switch (operation.opcode) {
                case OpCode::Nop:
                case OpCode::LoadImm:
                case OpCode::FaultLocation:
                case OpCode::GuestSafepoint:
                case OpCode::EntryPoint:
                    break;
                default:
                    if (!RecordInputUses(i, inputs, pc_literal, selection)) return false;
            }
            ObserveNZCV(i, inputs, effects, selection);
            SelectPinnedHome(i, inputs, effects, selection);
        }
    }
    return true;
}
}  // namespace swift::runtime::backend::arm64::detail
