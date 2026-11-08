#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {
bool Writer::DeferTest(const Operation& operation, Cursor& cursor) {
    const auto& body = source.Blocks()[block.value];
    const bool block_tail = source.ScopeKind() == Scope::Block;
    if (!deferred_test.value || deferred_test.site_count != 2 ||
        node.value != body.first_node + body.node_count - (block_tail ? 4u : 3u) ||
        (operation.type != Type::U32 && operation.type != Type::U64) ||
        (block_tail && operation.type != Type::U64) ||
        operation.Metadata() != OperationMetadata{all_flags, FlagProfile::LogicalZeroCVA, false})
        return false;
    auto at = cursor.at;
    const auto left = *at++, right = *at++;
    const auto same_value = [&](CellView value) {
        return value.Kind() == OperandKind::Value && value.Payload() == deferred_test.value.value &&
               (value.TypeView() == Type::Void || value.TypeView() == operation.type);
    };
    if (at != cursor.end || !same_value(left) || !same_value(right) ||
        source.Operations()[deferred_test.value.value].type != operation.type)
        return false;

    // Both forms publish the previous pointer and read NE. A Block then
    // selects its actual self/exit PC. Validate the complete tail before
    // suppressing TEST; the emitted prefix needs no rollback on a mismatch.
    const auto tail_store = NodeId{node.value + 1}, condition = NodeId{node.value + 2};
    const auto& store = source.Operations()[tail_store.value];
    const auto& check = source.Operations()[condition.value];
    if (store.opcode != OpCode::StoreUniform || store.Metadata() != OperationMetadata{} ||
        check.opcode != OpCode::CondSet || check.Metadata() != OperationMetadata{} ||
        source.Operands(condition).front().Payload() != static_cast<unsigned>(ir::Condition::NE))
        return false;
    if (block_tail) {
        const auto dispatch = NodeId{condition.value + 1};
        const auto& select = source.Operations()[dispatch.value];
        if (select.opcode != OpCode::StoreUniformSelectImm ||
            select.Metadata() != OperationMetadata{})
            return false;
        const auto cells = source.Operands(dispatch);
        auto pc = cells.begin();
        const auto field = *pc++, predicate = *pc++, yes = *pc++, no = *pc++;
        if (pc != cells.end() || field.Kind() != OperandKind::Uniform ||
            field.TypeView() != Type::U64 || field.Payload() != options.pc_offset ||
            predicate.Kind() != OperandKind::Value || predicate.Payload() != condition.value ||
            (predicate.TypeView() != Type::Void && predicate.TypeView() != check.type) ||
            yes.Kind() != OperandKind::Immediate || no.Kind() != OperandKind::Immediate ||
            yes.TypeView() != Type::U64 || no.TypeView() != Type::U64 ||
            yes.Payload() != options.entry_pc || no.Payload() == options.entry_pc)
            return false;
    }
    auto input = source.Operands(tail_store).begin();
    const auto field = *input++, value = *input;
    if (field.TypeView() != Type::U64 || !TestField(field) ||
        field.Payload() == deferred_test.snapshot_offset ||
        field.Payload() == deferred_test.capture_offset || value.Kind() != OperandKind::Value ||
        value.Payload() != deferred_test.snapshot.value ||
        (value.TypeView() != Type::Void && value.TypeView() != Type::U64))
        return false;
    const auto& snapshot_home = allocation.homes[deferred_test.snapshot.value];
    const auto& value_home = allocation.homes[deferred_test.value.value];
    const auto& test_home = allocation.homes[node.value];
    if (snapshot_home.reg != deferred_test.snapshot_register || snapshot_home.narrow_pin ||
        snapshot_home.spilled || snapshot_home.last_use < test_home.definition_position ||
        value_home.reg != deferred_test.value_register || value_home.narrow_pin ||
        value_home.spilled || test_home.last_use != test_home.definition_position ||
        !allocation.homes[condition.value].terminal_only)
        return false;

    deferred_test.bits = static_cast<std::uint8_t>(TypeBits(operation.type));
    for (const auto site : deferred_test.sites) {
        output.faults[site].recovery[0] = deferred_test.bits;
        output.faults[site].recovery[1] = deferred_test.snapshot_register;
    }
    cursor.at = at;
    deferred_test_used = true;
    deferred_parity = 0;
    direct_carry = false;  // The qualified TEST replaces all ordinary local flags.
    Immediate(29, abi::DeferredTestTag);
    shadow_dirty = true;
    packed_current = false;
    nzcv_matches = 0;
    return true;
}

void Writer::CompleteDeferredTest() {
    // Each terminal edge establishes its own complete flags. This fixed
    // cold/zero emission must not change the other edge's bookkeeping.
    Move(1, deferred_test.value_register);
    __ orr(v::w29,
           v::wzr,
           Operand(v::w1));  // MOV w29,w1: AF=(w29^w1).bit4 is zero and the tag is gone.
    __ cmp(X(deferred_test.value_register),
           Operand(0));  // CMP actual next,#0; inverted C=1.
}

void Writer::FlagsMemory(bool load, unsigned value) {
    const auto offset = options.flags_offset;
    if (!(offset & 7) && offset / 8 <= 4095)
        Memory(load, 64, value, 19, offset / 8);
    else {
        const unsigned address = value == 17 ? 30 : 17;
        Immediate(address, offset);
        __ add(X(address), v::x19, Operand(X(address)));
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
    } else if (RawAF())
        UnpackRawFlags();
    shadow_dirty = false;
}

void Writer::HelperFPEnvironment(bool guest) {
    if (!Linked() || !options.native_fp_environment) return;
    if (!guest) {
        __ mrs(v::x30, v::FPSR);
        ContextMemory(false, 30, abi::ContextOffset(SVM_A64_CTX_GUEST_FPSR));
    }
    ContextMemory(
            true, 30, abi::ContextOffset(guest ? SVM_A64_CTX_GUEST_FPCR : SVM_A64_CTX_HOST_FPCR));
    __ msr(v::FPCR, v::x30);
    ContextMemory(
            true, 30, abi::ContextOffset(guest ? SVM_A64_CTX_GUEST_FPSR : SVM_A64_CTX_HOST_FPSR));
    __ msr(v::FPSR, v::x30);
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
        __ eor(v::x17, v::x29, Operand(v::x0));
        Extract(17, 17, 4, 1);
        Insert(1, 17, 26, 1);
    } else if (packed_current)
        return;
    __ mrs(v::x17, v::NZCV);                        // MRS x17,NZCV.
    __ eor(v::x17, v::x17, Operand(u64{1} << 29));  // EOR x17,x17,#(1<<29): hardware C is inverted.
    Extract(17, 17, 28, 4);
    Insert(1, 17, 28, 4);
    packed_current = true;
}

void Writer::UnpackRawFlags() {
    Extract(29, 1, 22, 5);  // Packed AF becomes bit4 before the PF xor.
    __ eor(v::w29, v::w29, Operand(v::w1));
    Extract(1, 1, 0, 8);
    deferred_parity = 0;
    packed_current = false;
}

void Writer::RestoreNZCV() {
    if (!NativeNZCV()) return;
    __ eor(v::x17, v::x1, Operand(u64{1} << 29));  // EOR x17,x1,#(1<<29).
    __ msr(v::NZCV, v::x17);                       // MSR NZCV,x17.
    direct_carry = false;
    if (RawAF())
        UnpackRawFlags();
    else
        packed_current = true;
}

void Writer::RestoreHelperNZCV() {
    if (!RawAF()) {
        RestoreNZCV();
        return;
    }
    // The helper preserves raw x1/x29; its real observer shadow was
    // committed before BLR. Restore PSTATE without changing either carrier.
    FlagsMemory(true, 17);
    __ eor(v::x17, v::x17, Operand(u64{1} << 29));
    __ msr(v::NZCV, v::x17);
    deferred_parity = 0;
    direct_carry = false;
    packed_current = false;
}

void Writer::EnsureNZCV(unsigned condition) {
    if (NativeNZCV()) return;
    static constexpr u64 required[] = {zero,
                                       zero,
                                       carry,
                                       carry,
                                       negative,
                                       negative,
                                       overflow,
                                       overflow,
                                       carry | zero,
                                       carry | zero,
                                       negative | overflow,
                                       negative | overflow,
                                       negative | overflow | zero,
                                       negative | overflow | zero,
                                       0,
                                       0};
    if ((nzcv_matches & required[condition]) != required[condition]) {
        __ msr(v::NZCV,
               v::x1);  // MSR NZCV,x1, only when this condition needs stale host bits.
        nzcv_matches = nzcv_flags;
    }
}

void Writer::ClearMask(u64 mask) {
    CompleteLocalFlags();
    nzcv_matches &= ~mask;
    const auto nzcv = HostMask(mask);
    if (NativeNZCV() && nzcv) {
        Rmif(31, 0, nzcv >> 28);
        if (mask & carry) __ cfinv();  // Guest C=0 means hardware C=1.
        packed_current = false;
    }
    if (RawAF()) {
        if ((mask & parity) && !(mask & auxiliary))
            __ eor(v::w29, v::w29, Operand(v::w1));  // Preserve AF while PF.bit4 becomes zero.
        if (mask & parity) Immediate(1, 1);          // Odd raw byte represents cleared PF.
        if (mask & auxiliary)
            __ orr(v::w29, v::wzr, Operand(v::w1));  // AF=0: xor input matches PF.
        packed_current = false;
        return;
    }
    const auto host = (NativeNZCV() ? 0u : nzcv) | ((mask & auxiliary) ? 1u << 26 : 0);
    for (unsigned bit = 26; bit <= 31;) {
        if (!(host & (1u << bit))) {
            ++bit;
            continue;
        }
        const auto first = bit;
        while (bit <= 31 && (host & (1u << bit))) ++bit;
        Insert(1, 31, first, bit - first);
    }
    if (mask & parity) {
        Immediate(17, 1);
        Insert(1, 17, 0, 8);
    }
}

void Writer::MergeNZCV(u64 mask, bool flip_carry) {
    const auto host = HostMask(mask);
    if (!host) return;
    __ mrs(v::x17, v::NZCV);  // MRS x17,NZCV; x1 remains the authoritative guest word.
    if (flip_carry && (mask & carry)) {
        __ eor(v::x17, v::x17, Operand(u64{1} << 29));  // EOR x17,x17,#(1<<29).
    }
    for (unsigned bit = 28; bit <= 31;) {
        if (!(host & (1u << bit))) {
            ++bit;
            continue;
        }
        const auto first = bit;
        while (bit <= 31 && (host & (1u << bit))) ++bit;
        Extract(0, 17, first, bit - first);
        Insert(1, 0, first, bit - first);
    }
    nzcv_matches = (nzcv_matches & ~mask) | (mask & nzcv_flags & ~(flip_carry ? carry : 0));
}

void Writer::Parity(unsigned to, unsigned value) {
    Extract(to, value, 0, 8);
    for (const unsigned shift : {4u, 2u, 1u}) __ eor(X(to), X(to), Operand(X(to), v::LSR, shift));
    Narrow(to, to, 1);
    __ eor(X(to), X(to), Operand(1));  // EOR Xd,Xd,#1.
}

void Writer::Test(u64 mask, unsigned destination, bool inverse) {
    CompleteLocalFlags();
    if (!RawAF()) PackNZCV();
    bool first = true;
    if (const auto host = HostMask(mask)) {
        if (RawAF()) {
            __ mrs(v::x16, v::NZCV);  // MRS x16,NZCV; raw PF/AF remain untouched.
            if (mask & carry) __ eor(v::x16, v::x16, Operand(u64{1} << 29));
        }
        Immediate(17, host);
        __ and_(v::x16, RawAF() ? X(16) : X(1), Operand(v::x17));
        Predicate(0, 16, false);
        first = false;
    }
    if (mask & parity) {
        Parity(16, 1);
        if (first)
            Move(0, 16);
        else
            __ and_(v::x0, v::x0, Operand(v::x16));
        first = false;
    }
    if (mask & auxiliary) {
        if (RawAF()) {
            __ eor(v::x16, v::x29, Operand(v::x1));
            Extract(16, 16, 4, 1);
        } else
            Extract(16, 1, 26, 1);
        if (first)
            Move(0, 16);
        else
            __ and_(v::x0, v::x0, Operand(v::x16));
        first = false;
    }
    if (first) Immediate(0, 0);
    if (inverse) __ eor(v::x0, v::x0, Operand(1));  // EOR x0,x0,#1.
    Move(destination, 0);
}

void Writer::Metadata(const Operation& op) const {
    const auto value = op.Metadata();
    if (value == OperationMetadata{}) return;
    RequireFlags();
    if (!value.flag_outputs || (value.flag_outputs & ~all_flags) || value.branch_only ||
        (op.type != Type::U8 && op.type != Type::U16 && op.type != Type::U32 &&
         op.type != Type::U64))
        Reject(CompileError::UnsupportedFlags);
    if (value.profile == FlagProfile::ArithmeticRaw ||
        value.profile == FlagProfile::ArithmeticCanonicalCarry) {
        if (!Arithmetic(op.opcode)) Reject(CompileError::UnsupportedFlags);
    } else if (value.profile != FlagProfile::LogicalZeroCVA || !Logical(op.opcode))
        Reject(CompileError::UnsupportedFlags);
}

u64 Writer::LegacyMask(NodeId owner, NodeId save) const {
    const auto operations = source.Operations();
    if (owner.value >= operations.size() || save.value >= operations.size() ||
        save.value <= owner.value || save.value - owner.value > 2)
        return 0;
    const auto& producer = operations[owner.value];
    const auto& publication = operations[save.value];
    if (producer.block != publication.block || publication.opcode != OpCode::SaveFlags ||
        (!Arithmetic(producer.opcode) && !Logical(producer.opcode)))
        return 0;
    const auto width = Bits(producer.type);
    if (width != 8 && width != 16 && width != 32 && width != 64) return 0;
    const auto cells = source.Operands(save);
    auto at = cells.begin();
    if (at == cells.end()) return 0;
    const auto value = *at++;
    if (value.Kind() != OperandKind::Value || value.Payload() != owner.value || at == cells.end())
        return 0;
    const auto mask = *at++;
    if (at != cells.end() || mask.Kind() != OperandKind::Flags || (mask.Payload() & ~all_flags))
        return 0;
    if (save.value - owner.value == 2) {
        const auto& middle = operations[owner.value + 1];
        if (middle.block != producer.block || middle.opcode != OpCode::ClearFlags ||
            middle.Metadata() != OperationMetadata{})
            return 0;
        const auto clear = source.Operands(NodeId{owner.value + 1});
        auto c = clear.begin();
        if (c == clear.end()) return 0;
        const auto bits = *c++;
        if (c != clear.end() || bits.Kind() != OperandKind::Flags ||
            (bits.Payload() & ~all_flags) || (bits.Payload() & mask.Payload()))
            return 0;
    }
    return mask.Payload();
}

u64 Writer::CaptureLegacy(const Operation& op) {
    if (!Enabled() || (!Arithmetic(op.opcode) && !Logical(op.opcode))) return 0;
    NodeId save{node.value + 1};
    if (save.value < source.Operations().size() &&
        source.Operations()[save.value].opcode == OpCode::ClearFlags)
        ++save.value;
    const auto mask = LegacyMask(node, save);
    if (mask) {
        if (pending_save || op.Metadata() != OperationMetadata{})
            Reject(CompileError::UnsupportedFlags);
        pending_save = save;
    }
    return mask;
}

void Writer::WriteParity(unsigned result, bool preserve_af) {
    if (RawAF()) {
        const auto& home = allocation.homes[node.value];
        const bool pinned =
                Pinned() &&
                ((result >= 20 && result < 28) ||
                 (options.pinned_uniform_slots > 8 &&
                  result >= (options.pinned_uniform_slots == 16 ? 8u : 12u) && result < 16));
        if (!preserve_af && CanDeferFlags() && pinned && !home.spilled && result == home.reg &&
            (source.Operations()[node.value].Metadata().flag_outputs & (parity | auxiliary)) ==
                    (parity | auxiliary)) {
            deferred_parity = result;
            packed_current = false;
            return;
        }
        CompleteParity();
        if (preserve_af) {
            __ eor(v::w29, v::w29, Operand(v::w1));
            __ eor(v::w29, v::w29, Operand(W(result)));
        }
        Move(1, result);
        packed_current = false;
    } else
        Insert(1, result, 0, 8);
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
                __ eor(v::w29, v::w29, Operand(W(result)));
                __ eor(v::w29, v::w29, Operand(v::w1));
            }
            packed_current = false;
        } else {
            __ eor(v::x0, v::x0, Operand(X(result)));
            Extract(0, 0, 4, 1);
            Insert(1, 0, 26, 1);
        }
    }
    if (mask & parity) WriteParity(result, !(mask & auxiliary));
    if (NativeNZCV())
        FinishNZCV(mask, !canonical);
    else
        MergeNZCV(mask, canonical);
}

void Writer::LogicalFlags(unsigned result,
                          unsigned width,
                          u64 mask,
                          bool clear_af,
                          bool nzcv_ready,
                          bool invert_carry) {
    if (RawAF() && clear_af && (width == 8 || width == 16) && mask == all_flags && !nzcv_ready) {
        const auto& home = allocation.homes[node.value];
        if (result != 1 || home.last_use == home.definition_position) {
            // Only the PF carrier becomes signed. Keep a live canonical
            // numerical result untouched; the same low byte supplies PF.
            Narrow(1, result, width, true);
            result = 1;
            width = 64;
        }
    }
    if (mask & parity) WriteParity(result, !(clear_af && (mask & auxiliary)));
    if (NativeNZCV()) {
        if (clear_af && (mask & auxiliary)) {
            if (RawAF()) {
                __ orr(v::w29, v::wzr, Operand(v::w1));
                packed_current = false;
            } else
                Insert(1, 31, 26, 1);
        }
        if (mask & (negative | zero)) {
            if (!nzcv_ready) {
                auto value = result;
                if (width < 32) {
                    Narrow(0, result, width, true);
                    value = 0;
                }
                PrepareNZCV(mask);
                __ cmp(R(value, width == 32 ? 32 : 64),
                       Operand(0));  // CMP sets the native inverted C=1.
            }
            FinishNZCV(mask, nzcv_ready && invert_carry);  // Ready ANDS needs inversion; CMP/SUBS
                                                           // can already supply it.
        } else
            ClearMask(mask & (carry | overflow));
        return;
    }
    ClearMask(mask & (carry | overflow | (clear_af ? auxiliary : 0)));
    if (mask & (negative | zero)) {
        if (!nzcv_ready) {
            auto value = result;
            if (width < 32) {
                Narrow(0, result, width, true);
                value = 0;
            }
            nzcv_matches = 0;
            __ tst(R(value, width == 32 ? 32 : 64),
                   Operand(R(value,
                             width == 32 ? 32 : 64)));  // TST at the architectural sign width.
        }
        MergeNZCV(mask & (negative | zero));
        nzcv_matches |=
                mask &
                (carry | overflow);  // ANDS/TST and the declared logical write both clear C/V.
    }
}

void Writer::HandleGetFlagsFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    RequireFlags();
    if (cursor.Take().Kind() != OperandKind::Value) Reject(CompileError::UnsupportedOperand);
    Flags(cursor.Take(), false);
    PackNZCV();
    Move(destination, 1);  // Whole raw word; mask is not a filter.
    if (RawAF()) UnpackRawFlags();
}

void Writer::HandleRestoreFlagsFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    RequireFlags();
    PackNZCV();
    const auto input = RawScalar(cursor.Take(), 16);
    const auto mask = Flags(cursor.Take(), false);
    if (mask & ~(all_flags | FlagBits(Flag::Direction))) Reject(CompileError::UnsupportedFlags);
    nzcv_matches &= ~mask;
    if (mask & parity) Insert(1, input, 0, 8);
    if (mask & FlagBits(Flag::Direction)) {
        Extract(17, input, 10, 1);
        Insert(1, 17, 10, 1);
    }
    const auto host = HostMask(mask) | (mask & auxiliary ? 1u << 26 : 0);
    for (unsigned bit = 26; bit <= 31;) {
        if (!(host & (1u << bit))) {
            ++bit;
            continue;
        }
        const auto first = bit;
        while (bit <= 31 && (host & (1u << bit))) ++bit;
        Extract(17, input, first, bit - first);
        Insert(1, 17, first, bit - first);
    }
    // The raw-PF ABI carries reserved architectural bits only in
    // the real memory shadow. Publish DF before unpacking x1.
    if (RawAF() && (mask & FlagBits(Flag::Direction))) FlagsMemory(false, 1);
    if (mask & nzcv_flags)
        RestoreNZCV();
    else if (RawAF())
        UnpackRawFlags();
}

void Writer::HandleSaveFlagsFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    RequireFlags();
    const auto owner = cursor.Take();
    const auto mask = Flags(cursor.Take());
    if (owner.Kind() != OperandKind::Value) Reject(CompileError::UnsupportedFlags);
    if (mask && (pending_save != node ||
                 LegacyMask(NodeId{static_cast<u32>(owner.Payload())}, node) != mask))
        Reject(CompileError::UnsupportedFlags);
    if (pending_save == node) pending_save = {};
}

void Writer::HandleClearFlagsFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    RequireFlags();
    ClearMask(Flags(cursor.Take()));
}

void Writer::HandlePublishFCmpFlagsFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    CompleteLocalFlags();
    RequireFlags();
    const auto value = Read(cursor.Take(), 16, 32);
    ClearMask(overflow | negative | (RawAF() ? 0 : auxiliary));
    nzcv_matches = 0;
    if (NativeNZCV()) {
        Rmif(value, 63, 2);
        __ cfinv();         // Guest CF bit0 becomes inverted hardware C.
        Rmif(value, 0, 4);  // Guest ZF is neutral bit2.
        packed_current = false;
    } else {
        Extract(17, value, 0, 1);
        Insert(1, 17, 29, 1);
        Extract(17, value, 2, 1);
        Insert(1, 17, 30, 1);
    }
    Extract(17, value, 1, 1);
    __ eor(v::w17, v::w17, Operand(1));  // EOR W17,W17,#1: even raw byte means guest PF=1.
    if (RawAF()) {
        Move(1, 17);
        Move(29, 31);
        packed_current = false;
    } else
        Insert(1, 17, 0, 8);
}

void Writer::HandleTestFlagsFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    RequireFlags();
    const auto mask = Flags(cursor.Take());
    const bool inverse = operation.opcode == OpCode::TestNotFlags;
    if (home.terminal_only) {
        terminal_condition = mask == (carry | zero) ? 20u
                             : mask == carry        ? 2u
                             : mask == overflow     ? 6u
                             : mask == zero         ? 0u
                             : mask == negative     ? 4u
                                                    : 16u;
        terminal_condition ^= static_cast<unsigned>(inverse);
    } else if (NativeNZCV() && (mask == carry || mask == overflow || mask == zero ||
                                mask == negative || mask == (carry | zero))) {
        const auto condition = (mask == carry      ? 3u
                                : mask == overflow ? 6u
                                : mask == zero     ? 0u
                                : mask == negative ? 4u
                                                   : 9u) ^
                               static_cast<unsigned>(inverse);
        __ cset(X(destination), static_cast<v::Condition>(condition));
    } else
        Test(mask, destination, inverse);
}

void Writer::HandleSetCarryFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    if (node == elided_consumer) {
        cursor.Take();
        elided_consumer = {};
        return;
    }
    CompleteLocalFlags();
    RequireFlags();
    const auto value = Read(cursor.Take(), 16);
    if (NativeNZCV()) {
        const bool set_carry = operation.opcode == OpCode::SetCarry;
        Rmif(value, set_carry ? 63 : 0, set_carry ? 2 : 1);
        if (set_carry) __ cfinv();
        packed_current = false;
        return;
    }
    nzcv_matches &= ~(operation.opcode == OpCode::SetCarry ? carry : overflow);
    Insert(1, value, operation.opcode == OpCode::SetCarry ? 29 : 28, 1);
}

void Writer::HandleInvertCarryFamily(OpContext& /*ctx*/) {
    CompleteLocalFlags();
    RequireFlags();
    nzcv_matches &= ~carry;
    if (NativeNZCV()) {
        __ cfinv();
        packed_current = false;
    } else
        __ eor(v::x1, v::x1, Operand(u64{1} << 29));  // EOR x1,x1,#(1<<29).
}

void Writer::HandleLocalParitySetFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto input = Read(cursor.Take(), 16);
    const auto inverse = cursor.Take();
    if (inverse.Kind() != OperandKind::Immediate || inverse.Payload() > 1)
        Reject(CompileError::UnsupportedOperand);
    Parity(destination, input);
    if (inverse.Payload()) {
        Immediate(17, 1);
        __ eor(X(destination), X(destination), Operand(v::x17));
    }
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
