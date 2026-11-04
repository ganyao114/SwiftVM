#include "emit.h"

namespace swift::runtime::backend::arm64::detail {

bool Writer::DeferTest(const Operation& operation, Cursor& cursor) {
    const auto& body = source.Blocks()[block.value];
    const bool block_tail = source.ScopeKind() == Scope::Block;
    if (!deferred_test.value || deferred_test.site_count != 2 ||
            node.value != body.first_node + body.node_count - (block_tail ? 4u : 3u) ||
            (operation.type != Type::U32 && operation.type != Type::U64) ||
            (block_tail && operation.type != Type::U64) ||
            operation.Metadata() != OperationMetadata{all_flags, FlagProfile::LogicalZeroCVA, false}) return false;
    auto at = cursor.at;
    const auto left = *at++, right = *at++;
    const auto same_value = [&](CellView value) {
        return value.Kind() == OperandKind::Value && value.Payload() == deferred_test.value.value &&
                (value.TypeView() == Type::Void || value.TypeView() == operation.type);
    };
    if (at != cursor.end || !same_value(left) || !same_value(right) ||
            source.Operations()[deferred_test.value.value].type != operation.type) return false;

    // Both forms publish the previous pointer and read NE. A Block then
    // selects its actual self/exit PC. Validate the complete tail before
    // suppressing TEST; the emitted prefix needs no rollback on a mismatch.
    const auto tail_store = NodeId{node.value + 1}, condition = NodeId{node.value + 2};
    const auto& store = source.Operations()[tail_store.value];
    const auto& check = source.Operations()[condition.value];
    if (store.opcode != OpCode::StoreUniform || store.Metadata() != OperationMetadata{} ||
            check.opcode != OpCode::CondSet || check.Metadata() != OperationMetadata{} ||
            source.Operands(condition).front().Payload() != static_cast<unsigned>(Condition::NE)) return false;
    if (block_tail) {
        const auto dispatch = NodeId{condition.value + 1};
        const auto& select = source.Operations()[dispatch.value];
        if (select.opcode != OpCode::StoreUniformSelectImm || select.Metadata() != OperationMetadata{}) return false;
        const auto cells = source.Operands(dispatch);
        auto pc = cells.begin();
        const auto field = *pc++, predicate = *pc++, yes = *pc++, no = *pc++;
        if (pc != cells.end() || field.Kind() != OperandKind::Uniform || field.TypeView() != Type::U64 ||
                field.Payload() != options.pc_offset || predicate.Kind() != OperandKind::Value ||
                predicate.Payload() != condition.value ||
                (predicate.TypeView() != Type::Void && predicate.TypeView() != check.type) ||
                yes.Kind() != OperandKind::Immediate || no.Kind() != OperandKind::Immediate ||
                yes.TypeView() != Type::U64 || no.TypeView() != Type::U64 ||
                yes.Payload() != options.entry_pc || no.Payload() == options.entry_pc) return false;
    }
    auto input = source.Operands(tail_store).begin();
    const auto field = *input++, value = *input;
    if (field.TypeView() != Type::U64 || !TestField(field) ||
            field.Payload() == deferred_test.snapshot_offset || field.Payload() == deferred_test.capture_offset ||
            value.Kind() != OperandKind::Value || value.Payload() != deferred_test.snapshot.value ||
            (value.TypeView() != Type::Void && value.TypeView() != Type::U64)) return false;
    const auto& snapshot_home = allocation.homes[deferred_test.snapshot.value];
    const auto& value_home = allocation.homes[deferred_test.value.value];
    const auto& test_home = allocation.homes[node.value];
    if (snapshot_home.reg != deferred_test.snapshot_register || snapshot_home.narrow_pin || snapshot_home.spilled ||
            snapshot_home.last_use < test_home.definition_position || value_home.reg != deferred_test.value_register ||
            value_home.narrow_pin || value_home.spilled || test_home.last_use != test_home.definition_position ||
            !allocation.homes[condition.value].terminal_only) return false;

    deferred_test.bits = static_cast<std::uint8_t>(TypeBits(operation.type));
    for (const auto site : deferred_test.sites) {
        output.faults[site].test_bits = deferred_test.bits;
        output.faults[site].test_register = deferred_test.snapshot_register;
    }
    cursor.at = at;
    deferred_test_used = true;
    deferred_parity = 0; direct_carry = false; // The qualified TEST replaces all ordinary local flags.
    Immediate(29, abi::DeferredTestTag);
    shadow_dirty = true; packed_current = false; nzcv_matches = 0;
    return true;
}

void Writer::CompleteDeferredTest() {
    // Each terminal edge establishes its own complete flags. This fixed
    // cold/zero emission must not change the other edge's bookkeeping.
    Move(1, deferred_test.value_register);
    Put(0x2a0103fdu); // MOV w29,w1: AF=(w29^w1).bit4 is zero and the tag is gone.
    Put(0xf100001fu | (static_cast<u32>(deferred_test.value_register) << 5)); // CMP actual next,#0; inverted C=1.
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
            if (deferred_parity == value && (value != static_cast<unsigned>(pinned) || first)) CompleteParity();
            if (bits == 64) Move(value, static_cast<unsigned>(pinned));
            else Extract(value, static_cast<unsigned>(pinned), first, bits);
        } else {
            if (deferred_parity == static_cast<unsigned>(pinned) && !first &&
                    (bits != 64 || value != static_cast<unsigned>(pinned))) CompleteParity();
            if (bits == 64) Move(static_cast<unsigned>(pinned), value);
            else Insert(static_cast<unsigned>(pinned), value, first, bits);
        }
        return;
    }
    if (load && deferred_parity == value) CompleteParity();
    // A narrow or crossing field still uses its exact original memory access.
    // Publish the overlapping complete XMM state so untouched lanes survive.
    VectorAlias(false, offset, size);
    if (!(offset % size) && offset / size <= 4095) Memory(load, bits, value, 19, offset / size);
    else {
        Immediate(17, offset);
        Put(0x8b000000u | (17u << 16) | (19u << 5) | 17u);
        Memory(load, bits, value, 17);
    }
    if (!load) VectorAlias(true, offset, size);
}

void Writer::PublishPinned() {
    if (Pinned()) {
        const auto offset = options.pinned_uniform_offset;
        if (!(offset & 7) && (u64{offset} + (options.pinned_uniform_slots - 1u) * 8) / 8 <= 4095) {
            for (unsigned i = 0; i < options.pinned_uniform_slots; ++i)
                Memory(false, 64, abi::PinnedUniformRegister(i, options.pinned_uniform_slots), 19, offset / 8 + i);
        } else {
            Immediate(17, offset); Put(0x8b000000u | (17u << 16) | (19u << 5) | 17u);
            for (unsigned i = 0; i < options.pinned_uniform_slots; ++i)
                Memory(false, 64, abi::PinnedUniformRegister(i, options.pinned_uniform_slots), 17, i);
        }
    }
    if (VectorPinned()) VectorAlias(false, options.pinned_vector_offset, 256);
}

void Writer::FlagsMemory(bool load, unsigned value) {
    const auto offset = options.flags_offset;
    if (!(offset & 7) && offset / 8 <= 4095) Memory(load, 64, value, 19, offset / 8);
    else {
        const unsigned address = value == 17 ? 30 : 17;
        Immediate(address, offset); Put(0x8b000000u | (address << 16) | (19u << 5) | address);
        Memory(load, 64, value, address);
    }
}

void Writer::Shadow(bool load) {
    if (!Enabled()) return;
    CompleteLocalFlags();
    if (!load && RawAF() && !shadow_dirty) return;
    if (!load) PackNZCV();
    if (!load && !shadow_dirty) return;
    FlagsMemory(load, 1);
    if (load) {
        nzcv_matches = 0;
        RestoreNZCV();
    } else if (RawAF()) UnpackRawFlags();
    shadow_dirty = false;
}

void Writer::HelperFPEnvironment(bool guest) {
    if (!Linked() || !options.native_fp_environment) return;
    Memory(true, 64, 30, 28, guest ? 16u : 14u); // context guest/host FPCR.
    Put(0xd51b441eu); // MSR FPCR,x30.
    if (guest) Put(0xd51b443fu); // MSR FPSR,xzr: no MXCSR sticky-state model.
    else {
        Memory(true, 64, 30, 28, 15u); // context host FPSR.
        Put(0xd51b443eu); // MSR FPSR,x30.
    }
}

bool Writer::OverlapsShadow(CellView uniform) const {
    if (!Enabled()) return false;
    const auto bytes = TypeBits(uniform.TypeView()) / 8;
    return uniform.Payload() < u64{options.flags_offset} + 8 &&
            options.flags_offset < uniform.Payload() + bytes;
}

void Writer::PackNZCV() {
    if (!NativeNZCV()) return;
    CompleteLocalFlags();
    if (RawAF()) {
        // x1 is temporarily a full snapshot only inside this operation.
        // Keep the real packed shadow's reserved bits, never raw PF high bits.
        Move(0, 1);
        FlagsMemory(true, 1);
        Insert(1, 0, 0, 8);
        Put(0xca000000u | (0u << 16) | (29u << 5) | 17u);
        Extract(17, 17, 4, 1); Insert(1, 17, 26, 1);
    } else if (packed_current) return;
    Put(0xd53b4211u); // MRS x17,NZCV.
    Put(0xd2630231u); // EOR x17,x17,#(1<<29): hardware C is inverted.
    Extract(17, 17, 28, 4); Insert(1, 17, 28, 4);
    packed_current = true;
}

void Writer::UnpackRawFlags() {
    Extract(29, 1, 22, 5); // Packed AF becomes bit4 before the PF xor.
    Put(0x4a000000u | (1u << 16) | (29u << 5) | 29u);
    Extract(1, 1, 0, 8);
    deferred_parity = 0;
    packed_current = false;
}

void Writer::RestoreNZCV() {
    if (!NativeNZCV()) return;
    Put(0xd2630031u); // EOR x17,x1,#(1<<29).
    Put(0xd51b4211u); // MSR NZCV,x17.
    direct_carry = false;
    if (RawAF()) UnpackRawFlags();
    else packed_current = true;
}

void Writer::RestoreHelperNZCV() {
    if (!RawAF()) { RestoreNZCV(); return; }
    // The helper preserves raw x1/x29; its real observer shadow was
    // committed before BLR. Restore PSTATE without changing either carrier.
    FlagsMemory(true, 17);
    Put(0xd2630231u); Put(0xd51b4211u);
    deferred_parity = 0; direct_carry = false;
    packed_current = false;
}

void Writer::EnsureNZCV(unsigned condition) {
    if (NativeNZCV()) return;
    static constexpr u64 required[] = {zero, zero, carry, carry, negative, negative,
            overflow, overflow, carry | zero, carry | zero, negative | overflow,
            negative | overflow, negative | overflow | zero, negative | overflow | zero, 0, 0};
    if ((nzcv_matches & required[condition]) != required[condition]) {
        Put(0xd51b4201u); // MSR NZCV,x1, only when this condition needs stale host bits.
        nzcv_matches = nzcv_flags;
    }
}

void Writer::ClearMask(u64 mask) {
    CompleteLocalFlags();
    nzcv_matches &= ~mask;
    const auto nzcv = HostMask(mask);
    if (NativeNZCV() && nzcv) {
        Rmif(31, 0, nzcv >> 28);
        if (mask & carry) Put(0xd500401fu); // Guest C=0 means hardware C=1.
        packed_current = false;
    }
    if (RawAF()) {
        if ((mask & parity) && !(mask & auxiliary))
            Put(0x4a000000u | (1u << 16) | (29u << 5) | 29u); // Preserve AF while PF.bit4 becomes zero.
        if (mask & parity) Immediate(1, 1); // Odd raw byte represents cleared PF.
        if (mask & auxiliary) Put(0x2a0003e0u | (1u << 16) | 29u); // AF=0: xor input matches PF.
        packed_current = false;
        return;
    }
    const auto host = (NativeNZCV() ? 0u : nzcv) |
            ((mask & auxiliary) ? 1u << 26 : 0);
    for (unsigned bit = 26; bit <= 31;) {
        if (!(host & (1u << bit))) { ++bit; continue; }
        const auto first = bit; while (bit <= 31 && (host & (1u << bit))) ++bit;
        Insert(1, 31, first, bit - first);
    }
    if (mask & parity) { Immediate(17, 1); Insert(1, 17, 0, 8); }
}

void Writer::MergeNZCV(u64 mask, bool flip_carry) {
    const auto host = HostMask(mask);
    if (!host) return;
    Put(0xd53b4211u); // MRS x17,NZCV; x1 remains the authoritative guest word.
    if (flip_carry && (mask & carry)) {
        Put(0xd2630231u); // EOR x17,x17,#(1<<29).
    }
    for (unsigned bit = 28; bit <= 31;) {
        if (!(host & (1u << bit))) { ++bit; continue; }
        const auto first = bit; while (bit <= 31 && (host & (1u << bit))) ++bit;
        Extract(0, 17, first, bit - first); Insert(1, 0, first, bit - first);
    }
    nzcv_matches = (nzcv_matches & ~mask) | (mask & nzcv_flags & ~(flip_carry ? carry : 0));
}

void Writer::Parity(unsigned to, unsigned value) {
    Extract(to, value, 0, 8);
    for (const unsigned shift : {4u, 2u, 1u})
        Put(0xca400000u | (to << 16) | (shift << 10) | (to << 5) | to);
    Narrow(to, to, 1);
    Put(0xd2400000u | (to << 5) | to); // EOR Xd,Xd,#1.
}

void Writer::Test(u64 mask, unsigned destination, bool inverse) {
    CompleteLocalFlags();
    if (!RawAF()) PackNZCV();
    bool first = true;
    if (const auto host = HostMask(mask)) {
        if (RawAF()) {
            Put(0xd53b4210u); // MRS x16,NZCV; raw PF/AF remain untouched.
            if (mask & carry) Put(0xd2630210u);
        }
        Immediate(17, host);
        Put(0x8a000000u | (17u << 16) | ((RawAF() ? 16u : 1u) << 5) | 16u);
        Predicate(0, 16, false); first = false;
    }
    if (mask & parity) {
        Parity(16, 1);
        if (first) Move(0, 16); else Put(0x8a000000u | (16u << 16));
        first = false;
    }
    if (mask & auxiliary) {
        if (RawAF()) {
            Put(0xca000000u | (1u << 16) | (29u << 5) | 16u);
            Extract(16, 16, 4, 1);
        } else Extract(16, 1, 26, 1);
        if (first) Move(0, 16); else Put(0x8a000000u | (16u << 16));
        first = false;
    }
    if (first) Immediate(0, 0);
    if (inverse) Put(0xd2400000u); // EOR x0,x0,#1.
    Move(destination, 0);
}

void Writer::Metadata(const Operation& op) const {
    const auto value = op.Metadata();
    if (value == OperationMetadata{}) return;
    RequireFlags();
    if (!value.flag_outputs || (value.flag_outputs & ~all_flags) || value.branch_only ||
        (op.type != Type::U8 && op.type != Type::U16 && op.type != Type::U32 && op.type != Type::U64))
        Reject(CompileError::UnsupportedFlags);
    if (value.profile == FlagProfile::ArithmeticRaw || value.profile == FlagProfile::ArithmeticCanonicalCarry) {
        if (!Arithmetic(op.opcode)) Reject(CompileError::UnsupportedFlags);
    } else if (value.profile != FlagProfile::LogicalZeroCVA || !Logical(op.opcode) || op.opcode == OpCode::AndNot)
        Reject(CompileError::UnsupportedFlags);
}

u64 Writer::LegacyMask(NodeId owner, NodeId save) const {
    const auto operations = source.Operations();
    if (owner.value >= operations.size() || save.value >= operations.size() ||
        save.value <= owner.value || save.value - owner.value > 2) return 0;
    const auto& producer = operations[owner.value]; const auto& publication = operations[save.value];
    if (producer.block != publication.block || publication.opcode != OpCode::SaveFlags ||
        (!Arithmetic(producer.opcode) && !Logical(producer.opcode))) return 0;
    const auto width = Bits(producer.type);
    if (width != 8 && width != 16 && width != 32 && width != 64) return 0;
    const auto cells = source.Operands(save); auto at = cells.begin();
    if (at == cells.end()) return 0;
    const auto value = *at++;
    if (value.Kind() != OperandKind::Value || value.Payload() != owner.value || at == cells.end()) return 0;
    const auto mask = *at++;
    if (at != cells.end() || mask.Kind() != OperandKind::Flags || (mask.Payload() & ~all_flags)) return 0;
    if (save.value - owner.value == 2) {
        const auto& middle = operations[owner.value + 1];
        if (middle.block != producer.block || middle.opcode != OpCode::ClearFlags || middle.Metadata() != OperationMetadata{}) return 0;
        const auto clear = source.Operands(NodeId{owner.value + 1}); auto c = clear.begin();
        if (c == clear.end()) return 0;
        const auto bits = *c++;
        if (c != clear.end() || bits.Kind() != OperandKind::Flags ||
            (bits.Payload() & ~all_flags) || (bits.Payload() & mask.Payload())) return 0;
    }
    return mask.Payload();
}

u64 Writer::CaptureLegacy(const Operation& op) {
    if (!Enabled() || (!Arithmetic(op.opcode) && !Logical(op.opcode))) return 0;
    NodeId save{node.value + 1};
    if (save.value < source.Operations().size() && source.Operations()[save.value].opcode == OpCode::ClearFlags) ++save.value;
    const auto mask = LegacyMask(node, save);
    if (mask) {
        if (pending_save || op.Metadata() != OperationMetadata{}) Reject(CompileError::UnsupportedFlags);
        pending_save = save;
    }
    return mask;
}

void Writer::WriteParity(unsigned result, bool preserve_af) {
    if (RawAF()) {
        const auto& home = allocation.homes[node.value];
        const bool pinned = Pinned() && ((result >= 20 && result < 28) ||
                (options.pinned_uniform_slots > 8 && result >= (options.pinned_uniform_slots == 16 ? 8u : 12u) && result < 16));
        if (!preserve_af && CanDeferFlags() && pinned && !home.spilled && result == home.reg &&
                (source.Operations()[node.value].Metadata().flag_outputs & (parity | auxiliary)) == (parity | auxiliary)) {
            deferred_parity = result; packed_current = false; return;
        }
        CompleteParity();
        if (preserve_af) {
            Put(0x4a000000u | (1u << 16) | (29u << 5) | 29u);
            Put(0x4a000000u | (result << 16) | (29u << 5) | 29u);
        }
        Move(1, result);
        packed_current = false;
    } else Insert(1, result, 0, 8);
}

void Writer::ArithmeticFlags(unsigned result, u64 mask, bool canonical) {
    if (mask & auxiliary) {
        // The input xor was captured before any result/input alias:
        // raw mode keeps it in W29, packed mode in x0.
        if (RawAF()) {
            // x29 already holds the input xor. If PF is not updated, rebase
            // it onto the retained PF source instead of this result.
            if (!(mask & parity)) {
                CompleteParity();
                Put(0x4a000000u | (result << 16) | (29u << 5) | 29u);
                Put(0x4a000000u | (1u << 16) | (29u << 5) | 29u);
            }
            packed_current = false;
        } else {
            Put(0xca000000u | (result << 16)); Extract(0, 0, 4, 1); Insert(1, 0, 26, 1);
        }
    }
    if (mask & parity) WriteParity(result, !(mask & auxiliary));
    if (NativeNZCV()) FinishNZCV(mask, !canonical);
    else MergeNZCV(mask, canonical);
}

void Writer::LogicalFlags(unsigned result, unsigned width, u64 mask, bool clear_af, bool nzcv_ready, bool invert_carry) {
    if (RawAF() && clear_af && (width == 8 || width == 16) && mask == all_flags && !nzcv_ready) {
        const auto& home = allocation.homes[node.value];
        if (result != 1 || home.last_use == home.definition_position) {
            // Only the PF carrier becomes signed. Keep a live canonical
            // numerical result untouched; the same low byte supplies PF.
            Narrow(1, result, width, true);
            result = 1; width = 64;
        }
    }
    if (mask & parity) WriteParity(result, !(clear_af && (mask & auxiliary)));
    if (NativeNZCV()) {
        if (clear_af && (mask & auxiliary)) {
            if (RawAF()) { Put(0x2a0003e0u | (1u << 16) | 29u); packed_current = false; }
            else Insert(1, 31, 26, 1);
        }
        if (mask & (negative | zero)) {
            if (!nzcv_ready) {
                auto value = result;
                if (width < 32) { Narrow(0, result, width, true); value = 0; }
                PrepareNZCV(mask);
                Put((width == 32 ? 0x7100001fu : 0xf100001fu) | (value << 5)); // CMP sets the native inverted C=1.
            }
            FinishNZCV(mask, nzcv_ready && invert_carry); // Ready ANDS needs inversion; CMP/SUBS can already supply it.
        } else ClearMask(mask & (carry | overflow));
        return;
    }
    ClearMask(mask & (carry | overflow | (clear_af ? auxiliary : 0)));
    if (mask & (negative | zero)) {
        if (!nzcv_ready) {
            auto value = result;
            if (width < 32) { Narrow(0, result, width, true); value = 0; }
            nzcv_matches = 0;
            Put((width == 32 ? 0x6a00001fu : 0xea00001fu) |
                    (value << 16) | (value << 5)); // TST at the architectural sign width.
        }
        MergeNZCV(mask & (negative | zero));
        nzcv_matches |= mask & (carry | overflow); // ANDS/TST and the declared logical write both clear C/V.
    }
}

void Writer::EmitState(const Operation& operation, Cursor& cursor, unsigned destination,
                       const Home& home, bool test_prefix) {
    switch (operation.opcode) {
        case OpCode::LoadUniform: {
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
            if (home.narrow_pin) break; // The checked low8/16/32 snapshot borrows this unchanged pin.
            Uniform(true, uniform, destination); break;
        }
        case OpCode::StoreUniform: {
            const auto uniform = cursor.Take();
            const bool overlap = OverlapsShadow(uniform);
            if (overlap) Shadow(false); // Before materializing a possibly spilled store input.
            const auto input = cursor.Take();
            if (deferred_parity && uniform.TypeView() != Type::V128 && uniform.Payload() <= UINT32_MAX) {
                const auto size = Width(uniform.TypeView()) / 8;
                const auto pinned = PinnedSlot(static_cast<u32>(uniform.Payload()), size);
                if (pinned == static_cast<int>(deferred_parity) &&
                        !((uniform.Payload() - options.pinned_uniform_offset) & 7u)) {
                    const bool self = size == 8 && input.Kind() == OperandKind::Value &&
                            !allocation.homes[input.Payload()].spilled &&
                            allocation.homes[input.Payload()].reg == deferred_parity &&
                            std::min(Width(View(input)), allocation.homes[input.Payload()].CarrierBits()) >= 8;
                    if (!self) CompleteParity(); // Read(input,pinned) may itself overwrite the low byte.
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
                            node.value == deferred_test.snapshot.value + 1 && offset != deferred_test.snapshot_offset;
                    if (deferred_test.active) {
                        deferred_test.captured = true;
                        deferred_test.capture_offset = offset;
                        deferred_test.snapshot_register = static_cast<std::uint8_t>(abi::PinnedUniformRegister(
                                (offset - options.pinned_uniform_offset) / 8, options.pinned_uniform_slots));
                    }
                } else if (offset == deferred_test.capture_offset) deferred_test.active = false;
                else if (offset == deferred_test.snapshot_offset) {
                    deferred_test.active = deferred_test.active && !deferred_test.value &&
                            deferred_test.loaded && input.Payload() == deferred_test.loaded.value;
                    if (deferred_test.active) {
                        deferred_test.value = deferred_test.loaded;
                        deferred_test.value_register = static_cast<std::uint8_t>(abi::PinnedUniformRegister(
                                (offset - options.pinned_uniform_offset) / 8, options.pinned_uniform_slots));
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
                    Move(0, Read(input, 0)); // Read a spilled dynamic PC before retiring its frame.
                    // Deferred dispatch publishes x0 only on the cold resolver path.
                    if (!options.deferred_pc) PCMemory(false);
                }
                StackAdjust(false);
                if (static_pc_valid) TailJump(options.dispatcher, true, static_pc);
                else DynamicDispatch();
                dispatch_emitted = true;
                break;
            }
            if (uniform.TypeView() == Type::V128) VectorUniform(false, uniform, VectorValue(input, 0));
            else {
                // Full-slot publication can normalize directly into its
                // actual destination. Partial stores still need scratch
                // so narrowing cannot destroy the slot's preserved bits.
                const auto pinned = uniform.TypeView() == Type::U64
                        ? PinnedSlot(static_cast<u32>(uniform.Payload()), 8) : -1;
                const auto bits = Width(uniform.TypeView());
                // The actual BFXIL/byte/half/word store cuts these bits.
                // A narrower logical view or borrowed carrier still needs
                // Read to zero-extend before a wider field is written.
                const bool field_cuts = bits < 64 && input.Kind() == OperandKind::Value &&
                        std::min(Width(View(input)), allocation.homes[input.Payload()].CarrierBits()) >= bits;
                const auto value = field_cuts ? RawValue(static_cast<u32>(input.Payload()), 16, true) :
                        Read(input, pinned >= 0 ? static_cast<unsigned>(pinned) : 16);
                Uniform(false, uniform, value);
            }
            if (overlap) Shadow(true);
            break;
        }
        case OpCode::StoreUniformSelectImm: {
            const auto uniform = cursor.Take(), check = cursor.Take();
            const auto yes = cursor.Take(), no = cursor.Take();
            if (uniform.TypeView() != Type::U64 || yes.TypeView() != Type::U64 || no.TypeView() != Type::U64)
                Reject(CompileError::UnsupportedType);
            const auto taken = IntegerImmediate(yes), fallthrough = IntegerImmediate(no);
            const auto& body = source.Blocks()[block.value];
            if (Linked() && uniform.Payload() == options.pc_offset &&
                node.value + 1 == body.first_node + body.node_count && body.terminal.kind == TerminalKind::Dispatch) {
                if (source.ScopeKind() == Scope::Block && deferred_test.bits == 64) {
                    Width(View(check));
                    const auto head = static_cast<u32>(output.words.size());
                    if (options.deferred_pc) {
                        Put(0xb5000000u | deferred_test.value_register); // Ready self edge patches this actual CBNZ.
                        CompleteDeferredTest(); // Zero falls through and completes its own flags.
                        StackAdjust(false);
                        PublishPCAndJump(fallthrough);
                        TailJump(options.dispatcher, true, taken, head); // Unresolved/far self gate stays patchable.
                    } else {
                        Put(0xb4000000u | deferred_test.value_register);
                        PublishPCAndJump(taken); // Eager PC publication remains on the actual selected edge.
                        output.words[head] |= (static_cast<u32>(output.words.size()) - head) << 5;
                        CompleteDeferredTest();
                        StackAdjust(false);
                        PublishPCAndJump(fallthrough);
                    }
                    dispatch_emitted = true;
                    break;
                }
                CompleteLocalFlags();
                if (!options.private_uniforms) Shadow(false);
                const bool direct = check.Kind() == OperandKind::Value && allocation.homes[check.Payload()].terminal_only;
                if (direct) Width(View(check)); // Keep the original numeric-view admission.
                else { const auto value = Read(check, 16); Move(16, value); }
                StackAdjust(false);
                if (direct) ConditionPC(terminal_condition, taken, fallthrough);
                else if (taken == fallthrough) PublishPCAndJump(taken);
                else {
                    const auto skip = static_cast<u32>(output.words.size()); Put(0xb4000010u); // CBZ x16,no.
                    PublishPCAndJump(taken);
                    output.words[skip] |= (static_cast<u32>(output.words.size()) - skip) << 5;
                    PublishPCAndJump(fallthrough);
                }
                dispatch_emitted = true;
            } else {
                const bool overlap = OverlapsShadow(uniform);
                if (overlap) Shadow(false);
                SnapshotCompareNZCV();
                const auto value = Read(check, 0);
                Immediate(16, taken); Immediate(17, fallthrough);
                nzcv_matches = 0;
                Put(0xf100001fu | (value << 5));
                Put(0x9a800000u | (17u << 16) | (1u << 12) | (16u << 5) | 16u);
                RestoreCompareNZCV();
                Uniform(false, uniform, 16);
                if (overlap) Shadow(true);
            }
            break;
        }
        case OpCode::GetFlags: {
            RequireFlags();
            if (cursor.Take().Kind() != OperandKind::Value) Reject(CompileError::UnsupportedOperand);
            Flags(cursor.Take(), false); PackNZCV(); Move(destination, 1);
            if (RawAF()) UnpackRawFlags();
            break; // Whole raw word; mask is not a filter.
        }
        case OpCode::RestoreFlags: {
            RequireFlags(); PackNZCV();
            const auto input = RawScalar(cursor.Take(), 16); const auto mask = Flags(cursor.Take());
            nzcv_matches &= ~mask;
            if (mask & parity) Insert(1, input, 0, 8);
            const auto host = HostMask(mask) | (mask & auxiliary ? 1u << 26 : 0);
            for (unsigned bit = 26; bit <= 31;) {
                if (!(host & (1u << bit))) { ++bit; continue; }
                const auto first = bit; while (bit <= 31 && (host & (1u << bit))) ++bit;
                Extract(17, input, first, bit - first); Insert(1, 17, first, bit - first);
            }
            if (mask & nzcv_flags) RestoreNZCV();
            else if (RawAF()) UnpackRawFlags();
            break;
        }
        case OpCode::SaveFlags: {
            RequireFlags(); const auto owner = cursor.Take(); const auto mask = Flags(cursor.Take());
            if (owner.Kind() != OperandKind::Value) Reject(CompileError::UnsupportedFlags);
            if (mask && (pending_save != node || LegacyMask(NodeId{static_cast<u32>(owner.Payload())}, node) != mask))
                Reject(CompileError::UnsupportedFlags);
            if (pending_save == node) pending_save = {};
            break;
        }
        case OpCode::ClearFlags: RequireFlags(); ClearMask(Flags(cursor.Take())); break;
        case OpCode::PublishFCmpFlags: {
            CompleteLocalFlags();
            RequireFlags(); const auto value = Read(cursor.Take(), 16, 32);
            ClearMask(overflow | negative | (RawAF() ? 0 : auxiliary));
            nzcv_matches = 0;
            if (NativeNZCV()) {
                Rmif(value, 63, 2); Put(0xd500401fu); // Guest CF bit0 becomes inverted hardware C.
                Rmif(value, 0, 4); // Guest ZF is neutral bit2.
                packed_current = false;
            } else {
                Extract(17, value, 0, 1); Insert(1, 17, 29, 1);
                Extract(17, value, 2, 1); Insert(1, 17, 30, 1);
            }
            Extract(17, value, 1, 1);
            Put(0x52000000u | (17u << 5) | 17u); // EOR W17,W17,#1: even raw byte means guest PF=1.
            if (RawAF()) { Move(1, 17); Move(29, 31); packed_current = false; }
            else Insert(1, 17, 0, 8);
            break;
        }
        case OpCode::TestFlags: case OpCode::TestNotFlags: {
            RequireFlags(); const auto mask = Flags(cursor.Take());
            const bool inverse = operation.opcode == OpCode::TestNotFlags;
            if (home.terminal_only) {
                terminal_condition = mask == (carry | zero) ? 20u : mask == carry ? 2u : mask == overflow ? 6u :
                        mask == zero ? 0u : mask == negative ? 4u : 16u;
                terminal_condition ^= static_cast<unsigned>(inverse);
            } else if (NativeNZCV() && (mask == carry || mask == overflow || mask == zero ||
                    mask == negative || mask == (carry | zero))) {
                const auto condition = (mask == carry ? 3u : mask == overflow ? 6u : mask == zero ? 0u :
                        mask == negative ? 4u : 9u) ^ static_cast<unsigned>(inverse);
                Put(0x9a9f07e0u | ((condition ^ 1) << 12) | destination);
            } else Test(mask, destination, inverse);
            break;
        }
        case OpCode::SetCarry: case OpCode::SetOverflow: {
            CompleteLocalFlags();
            RequireFlags(); const auto value = Read(cursor.Take(), 16);
            if (NativeNZCV()) {
                const bool set_carry = operation.opcode == OpCode::SetCarry;
                Rmif(value, set_carry ? 63 : 0, set_carry ? 2 : 1);
                if (set_carry) Put(0xd500401fu);
                packed_current = false;
                break;
            }
            nzcv_matches &= ~(operation.opcode == OpCode::SetCarry ? carry : overflow);
            Insert(1, value, operation.opcode == OpCode::SetCarry ? 29 : 28, 1); break;
        }
        case OpCode::InvertCarry:
            CompleteLocalFlags();
            RequireFlags(); nzcv_matches &= ~carry;
            if (NativeNZCV()) { Put(0xd500401fu); packed_current = false; }
            else Put(0xd2630021u); // EOR x1,x1,#(1<<29).
            break;
        case OpCode::LocalParitySet: {
            const auto input = Read(cursor.Take(), 16); const auto inverse = cursor.Take();
            if (inverse.Kind() != OperandKind::Immediate || inverse.Payload() > 1) Reject(CompileError::UnsupportedOperand);
            Parity(destination, input);
            if (inverse.Payload()) { Immediate(17, 1); Put(0xca000000u | (17u << 16) | (destination << 5) | destination); }
            break;
        }
        default: Reject(CompileError::UnsupportedOpcode);
    }
}

} // namespace swift::runtime::backend::arm64::detail
