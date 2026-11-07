#include "emit.h"

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned, bool) noexcept;
std::uintptr_t CpuidHelper() noexcept;
std::uintptr_t X87Helper() noexcept;

namespace detail {
void Writer::PCMemory(bool load) {
    if (Linked() && !RawAF()) {
        Memory(load, 64, 0, 29);
        return;
    }
    const auto offset = options.pc_offset;
    if (!(offset & 7) && offset / 8 <= 4095)
        Memory(load, 64, 0, 19, offset / 8);
    else {
        Immediate(17, offset);
        masm.exact.add(v::x17, v::x19, Operand(v::x17));
        Memory(load, 64, 0, 17);
    }
}

bool Writer::ConstantPC(CellView value, u64& pc) const {
    if (value.Kind() != OperandKind::Value || Width(View(value)) != 64) return false;
    const auto& producer = source.Operations()[value.Payload()];
    if (producer.opcode != OpCode::LoadImm || producer.block != block || Bits(producer.type) != 64)
        return false;
    pc = source.Operands(NodeId{static_cast<u32>(value.Payload())}).front().Payload();
    return true;
}

void Writer::ObservePCStore(CellView uniform, CellView value) {
    if (!Linked()) return;
    const auto size = TypeBits(uniform.TypeView()) / 8;
    if (uniform.Payload() >= u64{options.pc_offset} + 8 ||
        options.pc_offset >= uniform.Payload() + size)
        return;
    static_pc_valid = false;
    alternate_pc_valid = false;
    if (uniform.Payload() != options.pc_offset || size != 8 || value.Kind() != OperandKind::Value ||
        Width(View(value)) != 64)
        return;
    if (ConstantPC(value, static_pc)) {
        static_pc_valid = true;
        return;
    }
    const auto& producer = source.Operations()[value.Payload()];
    if (producer.opcode == OpCode::Select && producer.block == block && Bits(producer.type) == 64) {
        auto at = source.Operands(NodeId{static_cast<u32>(value.Payload())}).begin();
        ++at;
        const auto yes = *at++, no = *at;
        static_pc_valid = ConstantPC(yes, static_pc) && ConstantPC(no, alternate_pc);
        alternate_pc_valid = static_pc_valid && static_pc != alternate_pc;
    }
}

void Writer::Uniform(bool load, CellView uniform, unsigned value) {
    if (uniform.Kind() != OperandKind::Uniform || uniform.Payload() > UINT32_MAX)
        Reject(CompileError::UnsupportedOperand);
    const auto bits = Width(uniform.TypeView()), size = bits / 8;
    if (!size) Reject(CompileError::UnsupportedType);
    const auto offset = static_cast<u32>(uniform.Payload());
    if (const auto pinned = PinnedSlot(offset, size); pinned >= 0) {
        const auto first = ((offset - options.pinned_uniform_offset) & 7u) * 8;
        if (load) {
            if (deferred_parity == value && (value != static_cast<unsigned>(pinned) || first))
                CompleteParity();
            if (bits == 64)
                Move(value, static_cast<unsigned>(pinned));
            else
                Extract(value, static_cast<unsigned>(pinned), first, bits);
        } else {
            if (deferred_parity == static_cast<unsigned>(pinned) && !first &&
                (bits != 64 || value != static_cast<unsigned>(pinned)))
                CompleteParity();
            if (bits == 64)
                Move(static_cast<unsigned>(pinned), value);
            else
                Insert(static_cast<unsigned>(pinned), value, first, bits);
        }
        return;
    }
    if (load && deferred_parity == value) CompleteParity();
    // A narrow or crossing field still uses its exact original memory access.
    // Publish the overlapping complete XMM state so untouched lanes survive.
    VectorAlias(false, offset, size);
    if (!(offset % size) && offset / size <= 4095)
        Memory(load, bits, value, 19, offset / size);
    else {
        Immediate(17, offset);
        masm.exact.add(v::x17, v::x19, Operand(v::x17));
        Memory(load, bits, value, 17);
    }
    if (!load) VectorAlias(true, offset, size);
}

void Writer::PublishPinned() {
    if (Pinned()) {
        const auto offset = options.pinned_uniform_offset;
        if (!(offset & 7) && (u64{offset} + (options.pinned_uniform_slots - 1u) * 8) / 8 <= 4095) {
            for (unsigned i = 0; i < options.pinned_uniform_slots; ++i)
                Memory(false,
                       64,
                       abi::PinnedUniformRegister(i, options.pinned_uniform_slots),
                       19,
                       offset / 8 + i);
        } else {
            Immediate(17, offset);
            masm.exact.add(v::x17, v::x19, Operand(v::x17));
            for (unsigned i = 0; i < options.pinned_uniform_slots; ++i)
                Memory(false,
                       64,
                       abi::PinnedUniformRegister(i, options.pinned_uniform_slots),
                       17,
                       i);
        }
    }
    if (VectorPinned()) VectorAlias(false, options.pinned_vector_offset, 256);
}

bool Writer::OverlapsShadow(CellView uniform) const {
    if (!Enabled()) return false;
    const auto bytes = TypeBits(uniform.TypeView()) / 8;
    return uniform.Payload() < u64{options.flags_offset} + 8 &&
           options.flags_offset < uniform.Payload() + bytes;
}

int Writer::VectorPinnedSlot(u32 offset, unsigned bytes) const {
    if (!VectorPinned() || bytes != 16 || (offset & 15) || offset < options.pinned_vector_offset ||
        u64{offset} + bytes > u64{options.pinned_vector_offset} + 256 ||
        ((offset - options.pinned_vector_offset) & 15))
        return -1;
    return static_cast<int>(
            abi::PinnedVectorRegister((offset - options.pinned_vector_offset) / 16));
}

void Writer::VectorAlias(bool reload, u32 offset, unsigned bytes) {
    if (!VectorPinned() || !bytes) return;
    const u64 begin = options.pinned_vector_offset, end = begin + 256;
    if (offset >= end || begin >= u64{offset} + bytes) return;
    const auto first = (std::max(u64{offset}, begin) - begin) / 16;
    const auto last = (std::min(u64{offset} + bytes, end) - 1 - begin) / 16;
    for (auto slot = first; slot <= last; ++slot) {
        const auto field = begin + slot * 16;
        const auto reg = abi::PinnedVectorRegister(static_cast<unsigned>(slot));
        if (!(field & 15) && field / 16 <= 4095)
            VectorMemory(reload, reg, 19, static_cast<u32>(field / 16));
        else {
            Immediate(17, field);
            masm.exact.add(v::x17, v::x19, Operand(v::x17));
            VectorMemory(reload, reg, 17);
        }
    }
}

void Writer::VectorUniform(bool load, CellView uniform, unsigned value) {
    if (uniform.Kind() != OperandKind::Uniform || uniform.Payload() > UINT32_MAX ||
        uniform.TypeView() != Type::V128)
        Reject(CompileError::UnsupportedOperand);
    const auto offset = static_cast<u32>(uniform.Payload());
    (void)PinnedSlot(offset, 16);  // Cross-slot vector accesses are outside this explicit ABI.
    if (const auto pinned = VectorPinnedSlot(offset, 16); pinned >= 0) {
        if (load)
            VectorMove(value, static_cast<unsigned>(pinned));
        else
            VectorMove(static_cast<unsigned>(pinned), value);
        return;
    }
    VectorAlias(false, offset, 16);
    if (!(offset & 15) && offset / 16 <= 4095)
        VectorMemory(load, value, 19, offset / 16);
    else {
        Immediate(17, offset);
        masm.exact.add(v::x17, v::x19, Operand(v::x17));
        VectorMemory(load, value, 17);
    }
    if (!load) VectorAlias(true, offset, 16);
}

void Writer::HandleUniformBarrierFamily(OpContext& /*ctx*/) {
    PublishPinned();
    masm.exact.dmb(v::InnerShareable, v::BarrierAll);
}

void Writer::HandleGetUniformAddressFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto offset = IntegerImmediate(cursor.Take());
    if (operation.type != Type::U64 || offset > UINT32_MAX)
        Reject(CompileError::UnsupportedOperand);
    if (offset <= 4095)
        masm.exact.add(X(destination), v::x19, Operand(offset));
    else {
        Immediate(destination, offset);
        masm.exact.add(X(destination), v::x19, Operand(X(destination)));
    }
}

void Writer::HandleLoadUniformFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    const auto vector = ctx.vector;
    const auto test_prefix = ctx.test_prefix;
    if (vector) {
        const auto uniform = cursor.Take();
        if (OverlapsShadow(uniform)) Shadow(false);
        VectorUniform(true, uniform, destination);
        return;
    }
    const auto uniform = cursor.Take();
    if (test_prefix) {
        deferred_test.active = (operation.type == Type::U32 || operation.type == Type::U64) &&
                               uniform.TypeView() == operation.type && TestField(uniform);
        if (deferred_test.active && !deferred_test.snapshot) {
            deferred_test.active = operation.type == Type::U64;
            if (deferred_test.active) {
                deferred_test.snapshot = node;
                deferred_test.snapshot_offset = static_cast<u32>(uniform.Payload());
            }
        }
    }
    if (uniform.TypeView() != operation.type) Reject(CompileError::UnsupportedType);
    if (OverlapsShadow(uniform)) Shadow(false);
    if (home.narrow_pin) return;  // The checked low8/16/32 snapshot borrows this unchanged pin.
    Uniform(true, uniform, destination);
}

void Writer::HandleStoreUniformFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto test_prefix = ctx.test_prefix;
    const auto uniform = cursor.Take();
    const bool overlap = OverlapsShadow(uniform);
    if (overlap) Shadow(false);  // Before materializing a possibly spilled store input.
    const auto input = cursor.Take();
    if (deferred_parity && uniform.TypeView() != Type::V128 && uniform.Payload() <= UINT32_MAX) {
        const auto size = Width(uniform.TypeView()) / 8;
        const auto pinned = PinnedSlot(static_cast<u32>(uniform.Payload()), size);
        if (pinned == static_cast<int>(deferred_parity) &&
            !((uniform.Payload() - options.pinned_uniform_offset) & 7u)) {
            const bool self = size == 8 && input.Kind() == OperandKind::Value &&
                              !allocation.homes[input.Payload()].spilled &&
                              allocation.homes[input.Payload()].reg == deferred_parity &&
                              std::min(Width(View(input)),
                                       allocation.homes[input.Payload()].CarrierBits()) >= 8;
            if (!self) CompleteParity();  // Read(input,pinned) may itself overwrite the low byte.
        }
    }
    if (test_prefix && deferred_test.snapshot && uniform.TypeView() == Type::U64 &&
        TestField(uniform) && input.Kind() == OperandKind::Value) {
        const auto& producer = source.Operations()[input.Payload()];
        const auto view = View(input);
        deferred_test.active = (producer.type == Type::U32 || producer.type == Type::U64) &&
                               IntegerType(view) && TypeBits(view) >= TypeBits(producer.type);
        const auto offset = static_cast<u32>(uniform.Payload());
        if (deferred_test.active && !deferred_test.captured) {
            deferred_test.active = input.Payload() == deferred_test.snapshot.value &&
                                   node.value == deferred_test.snapshot.value + 1 &&
                                   offset != deferred_test.snapshot_offset;
            if (deferred_test.active) {
                deferred_test.captured = true;
                deferred_test.capture_offset = offset;
                deferred_test.snapshot_register = static_cast<std::uint8_t>(
                        abi::PinnedUniformRegister((offset - options.pinned_uniform_offset) / 8,
                                                   options.pinned_uniform_slots));
            }
        } else if (offset == deferred_test.capture_offset)
            deferred_test.active = false;
        else if (offset == deferred_test.snapshot_offset) {
            deferred_test.active = deferred_test.active && !deferred_test.value &&
                                   deferred_test.loaded &&
                                   input.Payload() == deferred_test.loaded.value;
            if (deferred_test.active) {
                deferred_test.value = deferred_test.loaded;
                deferred_test.value_register = static_cast<std::uint8_t>(
                        abi::PinnedUniformRegister((offset - options.pinned_uniform_offset) / 8,
                                                   options.pinned_uniform_slots));
            }
        }
    }
    ObservePCStore(uniform, input);
    const auto& body = source.Blocks()[block.value];
    if (Linked() && uniform.TypeView() == Type::U64 && uniform.Payload() == options.pc_offset &&
        !alternate_pc_valid && node.value + 1 == body.first_node + body.node_count &&
        body.terminal.kind == TerminalKind::Dispatch) {
        CompleteLocalFlags();
        if (!options.private_uniforms) Shadow(false);
        if (!options.deferred_pc || !static_pc_valid) {
            Move(0, Read(input, 0));  // Read a spilled dynamic PC before retiring its frame.
            // Deferred dispatch publishes x0 only on the cold resolver path.
            if (!options.deferred_pc) PCMemory(false);
        }
        StackAdjust(false);
        if (static_pc_valid)
            TailJump(options.dispatcher, true, static_pc);
        else
            DynamicDispatch();
        dispatch_emitted = true;
        return;
    }
    if (uniform.TypeView() == Type::V128)
        VectorUniform(false, uniform, VectorValue(input, 0));
    else {
        // Full-slot publication can normalize directly into its
        // actual destination. Partial stores still need scratch
        // so narrowing cannot destroy the slot's preserved bits.
        const auto pinned = uniform.TypeView() == Type::U64
                                    ? PinnedSlot(static_cast<u32>(uniform.Payload()), 8)
                                    : -1;
        const auto bits = Width(uniform.TypeView());
        // The actual BFXIL/byte/half/word store cuts these bits.
        // A narrower logical view or borrowed carrier still needs
        // Read to zero-extend before a wider field is written.
        const bool field_cuts = bits < 64 && input.Kind() == OperandKind::Value &&
                                std::min(Width(View(input)),
                                         allocation.homes[input.Payload()].CarrierBits()) >= bits;
        const auto value = field_cuts
                                   ? RawValue(static_cast<u32>(input.Payload()), 16, true)
                                   : Read(input, pinned >= 0 ? static_cast<unsigned>(pinned) : 16);
        Uniform(false, uniform, value);
    }
    if (overlap) Shadow(true);
}

void Writer::HandleStoreUniformSelectImmFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto uniform = cursor.Take(), check = cursor.Take();
    const auto yes = cursor.Take(), no = cursor.Take();
    if (uniform.TypeView() != Type::U64 || yes.TypeView() != Type::U64 ||
        no.TypeView() != Type::U64)
        Reject(CompileError::UnsupportedType);
    const auto taken = IntegerImmediate(yes), fallthrough = IntegerImmediate(no);
    const auto& body = source.Blocks()[block.value];
    if (Linked() && uniform.Payload() == options.pc_offset &&
        node.value + 1 == body.first_node + body.node_count &&
        body.terminal.kind == TerminalKind::Dispatch) {
        if (source.ScopeKind() == Scope::Block && deferred_test.bits == 64) {
            Width(View(check));
            const auto head = static_cast<u32>(output.words.size());
            if (options.deferred_pc) {
                masm.exact.cbnz(X(deferred_test.value_register),
                                std::int64_t{0});  // Ready self edge patches this actual CBNZ.
                CompleteDeferredTest();  // Zero falls through and completes its own flags.
                StackAdjust(false);
                PublishPCAndJump(fallthrough);
                TailJump(options.dispatcher,
                         true,
                         taken,
                         head);  // Unresolved/far self gate stays patchable.
            } else {
                masm.exact.cbz(X(deferred_test.value_register), std::int64_t{0});
                PublishPCAndJump(taken);  // Eager PC publication remains on the actual
                                          // selected edge.
                masm.Patch(head, masm.Here());
                CompleteDeferredTest();
                StackAdjust(false);
                PublishPCAndJump(fallthrough);
            }
            dispatch_emitted = true;
            return;
        }
        CompleteLocalFlags();
        if (!options.private_uniforms) Shadow(false);
        const bool direct = check.Kind() == OperandKind::Value &&
                            allocation.homes[check.Payload()].terminal_only;
        if (direct)
            Width(View(check));  // Keep the original numeric-view admission.
        else {
            const auto value = Read(check, 16);
            Move(16, value);
        }
        StackAdjust(false);
        if (direct)
            ConditionPC(terminal_condition, taken, fallthrough);
        else if (taken == fallthrough)
            PublishPCAndJump(taken);
        else {
            const auto skip = static_cast<u32>(output.words.size());
            masm.exact.cbz(v::x16, std::int64_t{0});  // CBZ x16,no.
            PublishPCAndJump(taken);
            masm.Patch(skip, masm.Here());
            PublishPCAndJump(fallthrough);
        }
        dispatch_emitted = true;
    } else {
        const bool overlap = OverlapsShadow(uniform);
        if (overlap) Shadow(false);
        SnapshotCompareNZCV();
        const auto value = Read(check, 0);
        Immediate(16, taken);
        Immediate(17, fallthrough);
        nzcv_matches = 0;
        masm.exact.cmp(X(value), Operand(0));
        masm.exact.csel(v::x16, v::x16, v::x17, v::ne);
        RestoreCompareNZCV();
        Uniform(false, uniform, 16);
        if (overlap) Shadow(true);
    }
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64
