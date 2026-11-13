#include "abi_layout.inc"
#include "emit.h"
#include "runtime/exit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {

namespace detail {
void Writer::HandleGuestDispatchSafepointFamily(OpContext& ctx) {
    if (InterruptibleDispatch()) {
        (void)IntegerImmediate(ctx.cursor.Take());
        return;
    }
    HandleGuestSafepointFamily(ctx);
}

void Writer::HandleGuestSafepointFamily(OpContext& ctx) {
    if (!Linked() || options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    const auto resume_pc = IntegerImmediate(ctx.cursor.Take());
    ContextMemory(true, 16, abi::ContextOffset(SVM_A64_CTX_PENDING_EVENTS));
    if (!long_forward_branches && cold_exit_count < cold_exit_stubs.size()) {
        cold_exit_stubs[cold_exit_count++] = {masm.Here(),
                                              resume_pc,
                                              runtime::ExitValue(runtime::NativeExit::GuestEvents),
                                              nzcv_matches,
                                              static_cast<std::uint8_t>(deferred_parity),
                                              deferred_test.bits,
                                              deferred_test.value_register,
                                              true,
                                              shadow_dirty,
                                              packed_current,
                                              direct_carry};
        __ cbnz(v::x16, std::int64_t{0});
        return;
    }
    const auto skip = masm.Here();
    __ cbz(v::x16, std::int64_t{0});
    // This exit has no native successor. Its state publication must not change
    // the writer's knowledge of the ordinary, unexecuted fast-path shadow.
    const auto dirty = shadow_dirty, packed = packed_current;
    const auto matching = nzcv_matches;
    const auto parity_source = deferred_parity;
    const auto carry_source = direct_carry;
    // Only delivery observes flags. The zero-pending path keeps the existing
    // raw carriers; allocation still preserves every architectural flag.
    if (deferred_test.bits) CompleteDeferredTest();
    Shadow(false);
    Immediate(0, resume_pc);
    PCMemory(false);
    Immediate(0, runtime::ExitValue(runtime::NativeExit::GuestEvents));
    Epilogue();
    shadow_dirty = dirty;
    packed_current = packed;
    nzcv_matches = matching;
    deferred_parity = parity_source;
    direct_carry = carry_source;
    masm.Patch(skip, masm.Here());
}

void Writer::StackAdjust(bool subtract) {
    const auto spill_frame = output.frame_bytes - (Linked() ? 0 : 16);
    const u32 high = spill_frame >> 12, low = spill_frame & 4095;
    if (high) {
        if (subtract)
            __ sub(v::sp, v::sp, Operand(static_cast<u64>(high) << 12));
        else
            __ add(v::sp, v::sp, Operand(static_cast<u64>(high) << 12));
    }
    if (low) {
        if (subtract)
            __ sub(v::sp, v::sp, Operand(low));
        else
            __ add(v::sp, v::sp, Operand(low));
    }
}

void Writer::Branch(BlockId target) {
    if (target.value >= masm.labels.size()) Reject(CompileError::InvalidProgram);
    masm.fixups.push_back({static_cast<u32>(output.words.size()), target.value});
    __ b(std::int64_t{0});
}

bool Writer::ShortBranch(Branch19 instruction, BlockId target) {
    // B.cond and CBZ/CBNZ share imm19. A forward edge borrows the original
    // fixup list; only a genuine final range miss asks for long emission.
    if (masm.labels[target.value] == UINT32_MAX) {
        if (long_forward_branches) return false;
        masm.fixups.push_back({static_cast<u32>(output.words.size()), target.value});
        instruction.Emit(masm.exact, 0);
        return true;
    }
    const auto distance = static_cast<std::int64_t>(masm.labels[target.value]) -
                          static_cast<std::int64_t>(output.words.size());
    if (distance < -(std::int64_t{1} << 18) || distance >= (std::int64_t{1} << 18)) return false;
    instruction.Emit(masm.exact, distance);
    return true;
}

void Writer::Epilogue() {
    StackAdjust(false);
    if (Linked())
        TailJump(HostSymbol::Exit, options.exit_handler);
    else {
        __ ldp(v::x19, v::x30, MemOperand(v::sp, 16, v::PostIndex));
        __ ret(v::x30);
    }
}

unsigned Writer::PrepareTerminalCondition(unsigned condition, bool& restore, bool& flip) {
    if (condition == 16 || condition == 17) {
        SnapshotCompareNZCV();
        nzcv_matches = 0;
        if (RawAF()) {
            __ eor(v::x16, v::x29, Operand(v::x1));
            __ tst(v::x16, Operand(16));  // TST x16,#16: canonical AF is the xor's bit4.
        } else
            __ tst(v::x1, Operand(u64{1} << 26));  // TST x1,#(1<<26).
        condition = condition == 16 ? 1u : 0u;
        restore = NativeNZCV();
    } else if (NativeNZCV()) {
        if (condition == 20 || condition == 21)
            condition = 9u ^ (condition & 1u);  // Guest C|Z -> native LS.
        else if (condition == 2 || condition == 3)
            condition ^= 1u;
        else if (condition == 8 || condition == 9) {
            __ cfinv();
            flip = true;
        }
    } else
        EnsureNZCV(condition);
    return condition;
}

void Writer::RestoreTerminalCondition(bool restore, bool flip) {
    if (flip)
        __ cfinv();
    else if (restore)
        RestoreCompareNZCV();
}

void Writer::ConditionPC(unsigned condition, u64 taken, u64 fallthrough) {
    if (condition == 14 || condition == 15 || taken == fallthrough) {
        PublishPCAndJump(taken);
        return;
    }
    bool restore{}, flip{};
    condition = PrepareTerminalCondition(condition, restore, flip);
    if (options.deferred_pc && !restore && !flip) {
        // This external PC tail already completed local flags and retired
        // its frame. Its patchable link contract is the same in both scopes.
        // Backward/self yes edge takes priority; otherwise choose no. This is
        // only a static layout preference, not a guest fallthrough contract.
        const bool primary_yes = taken <= options.entry_pc;
        const auto primary = primary_yes ? taken : fallthrough;
        const auto other = primary_yes ? fallthrough : taken;
        const auto head = static_cast<u32>(output.words.size());
        __ b(std::int64_t{0}, static_cast<v::Condition>(condition ^ (primary_yes ? 0u : 1u)));
        PublishPCAndJump(other);
        TailJump(HostSymbol::Dispatcher, options.dispatcher, true, primary, head);
        return;
    }
    const auto skip = static_cast<u32>(output.words.size());
    __ b(std::int64_t{0}, static_cast<v::Condition>(condition ^ 1u));
    RestoreTerminalCondition(restore, flip);
    PublishPCAndJump(taken);
    masm.Patch(skip, masm.Here());
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
        const auto at = static_cast<u32>(output.words.size());
        __ b(std::int64_t{0}, static_cast<v::Condition>(skip));
        RestoreTerminalCondition(restore, flip);
        Branch(true_falls_through ? terminal.second : terminal.first);
        masm.Patch(at, masm.Here());
        RestoreTerminalCondition(restore, flip);
    } else if (!ShortBranch(Branch19::Flags(skip ^ 1u),
                            true_falls_through ? terminal.second : terminal.first)) {
        __ b(std::int64_t{2},
             static_cast<v::Condition>(skip));  // B.cond +8: only the actual long edge uses B26.
        Branch(true_falls_through ? terminal.second : terminal.first);
    }
    if (!true_falls_through && terminal.second != next) Branch(terminal.second);
}

void Writer::Terminal(const StoredTerminal& terminal, BlockId next) {
    CompleteLocalFlags();
    switch (terminal.kind) {
        case TerminalKind::Return:
            return HandleReturn(terminal);
        case TerminalKind::Dispatch:
            return HandleDispatch();
        case TerminalKind::Jump:
            return HandleJump(terminal, next);
        case TerminalKind::Branch:
            return HandleBranch(terminal, next);
        case TerminalKind::Unreachable:
            return HandleUnreachable();
        default:
            Reject(CompileError::UnsupportedControlFlow);
    }
}

void Writer::HandleReturn(const StoredTerminal& terminal) {
    Shadow(false);
    if (terminal.HasValue()) {
        const auto value = RawValue(terminal.value.value, 16);
        const auto type = terminal.value_type == Type::Void
                                  ? source.Operations()[terminal.value.value].type
                                  : terminal.value_type;
        Narrow(0, value, Width(type));
    } else
        Immediate(0, 0);
    Epilogue();
}

void Writer::HandleDispatch() {
    if (!options.private_uniforms) Shadow(false);
    if (Linked()) {
        StackAdjust(false);
        if (static_pc_valid && alternate_pc_valid) {
            // The selected PC is already committed. Use its two
            // real constant edges without extending the dead SSA
            // condition's lifetime or introducing a second cache.
            SnapshotCompareNZCV();
            PCMemory(true);
            Immediate(17, static_pc);
            __ cmp(v::x0, Operand(v::x17));
            const auto skip = static_cast<u32>(output.words.size());
            __ b(std::int64_t{0}, v::ne);
            RestoreCompareNZCV();
            TailJump(HostSymbol::Dispatcher, options.dispatcher, true, static_pc);
            masm.Patch(skip, masm.Here());
            RestoreCompareNZCV();
            TailJump(HostSymbol::Dispatcher, options.dispatcher, true, alternate_pc);
        } else {
            if (!options.deferred_pc || !static_pc_valid) PCMemory(true);
            if (static_pc_valid)
                TailJump(HostSymbol::Dispatcher, options.dispatcher, true, static_pc);
            else
                DynamicDispatch();
        }
    } else {
        Immediate(0, 0);
        Epilogue();
    }
}

void Writer::HandleJump(const StoredTerminal& terminal, BlockId next) {
    if (terminal.first != next) Branch(terminal.first);
}

void Writer::HandleBranch(const StoredTerminal& terminal, BlockId next) {
    const auto& home = allocation.homes[terminal.value.value];
    const auto type = terminal.value_type == Type::Void
                              ? source.Operations()[terminal.value.value].type
                              : terminal.value_type;
    if (deferred_test.bits) {
        Width(type);
        const auto& deferred = deferred_test;
        const auto cbnz =
                Branch19::Zero(R(deferred.value_register, deferred.bits == 64 ? 64 : 32), true);
        if (!ShortBranch(cbnz, terminal.first)) {
            cbnz.Inverse().Emit(masm.exact, 2);
            Branch(terminal.first);
        }
        // Only the zero exit completes TEST. Its current value,
        // not the nonzero entry snapshot, is known to be zero.
        Immediate(1, 0);
        Immediate(29, 0);
        __ cmp(v::x1, Operand(0));  // CMP x1,#0: N0 Z1 !guestC1 V0.
        shadow_dirty = true;
        packed_current = false;
        nzcv_matches = 0;
        if (terminal.second != next) Branch(terminal.second);
        return;
    }
    if (home.terminal_only) {
        Width(type);  // Preserve the terminal's scalar view contract.
        ConditionEdges(terminal_condition, terminal, next);
        return;
    }
    if (terminal.first == terminal.second) {
        if (terminal.first != next) Branch(terminal.first);
        return;
    }
    const auto value = RawValue(terminal.value.value, 16);
    Narrow(16, value, Width(type));
    const bool true_falls_through = terminal.first == next;
    if (!ShortBranch(Branch19::Zero(v::x16, !true_falls_through),
                     true_falls_through ? terminal.second : terminal.first)) {
        Branch19::Zero(v::x16, true_falls_through).Emit(masm.exact, 2);  // CBNZ/CBZ x16,+8
        Branch(true_falls_through ? terminal.second : terminal.first);
    }
    if (!true_falls_through && terminal.second != next) Branch(terminal.second);
}

void Writer::HandleUnreachable() {
    Shadow(false);
    __ brk(0);
}

void Writer::HandleCondSetFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    RequireFlags();
    const auto condition = Condition(cursor.Take());
    if (home.terminal_only) {
        terminal_condition = condition;
        return;
    }
    if (condition >= 14)
        Immediate(destination, 1);
    else {
        EnsureNZCV(condition);
        const bool flip = NativeNZCV() && (condition == 8 || condition == 9);
        const auto host =
                NativeNZCV() && (condition == 2 || condition == 3) ? condition ^ 1 : condition;
        if (flip) __ cfinv();
        __ cset(X(destination), static_cast<v::Condition>(host));
        if (flip) __ cfinv();
    }
}

void Writer::HandleCondSelectFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    RequireFlags();
    const auto condition = Condition(cursor.Take());
    const auto native_width = width == 32 ? 32u : 64u;
    const auto yes = Read(cursor.Take(), 16, 64, false, native_width),
               no = Read(cursor.Take(), 17, 64, false, native_width);
    if (condition >= 14) {
        Narrow(destination, yes, width);
        return;
    }
    EnsureNZCV(condition);
    const bool flip = NativeNZCV() && (condition == 8 || condition == 9);
    const auto host =
            NativeNZCV() && (condition == 2 || condition == 3) ? condition ^ 1 : condition;
    if (flip) __ cfinv();
    __ csel(R(destination, native_width),
            R(yes, native_width),
            R(no, native_width),
            static_cast<v::Condition>(host));
    if (flip) __ cfinv();
    if (width != 32) Narrow(destination, destination, width);
}

void Writer::HandleGotoFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    const auto save_result = [&] { SaveResult(ctx); };
    const auto input = cursor.Take();
    auto check = Read(input, 0);
    // The returned numeric value is available on both paths. Keep
    // the full input predicate even if a narrower result aliases it.
    if (destination == check && width < Width(View(input))) {
        Move(0, check);
        check = 0;
    }
    Narrow(destination, check, width);
    save_result();
    masm.local_fixups.push_back({node.value, static_cast<u32>(output.words.size())});
    Branch19::Zero(X(check), operation.opcode == OpCode::Goto).Emit(masm.exact, 0);
}

void Writer::HandleBindLabelFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto owner = cursor.Take();
    if (owner.Kind() != OperandKind::Value || owner.Payload() >= node.value)
        Reject(CompileError::UnsupportedOperand);
    const auto found = std::find_if(
            masm.local_fixups.begin(), masm.local_fixups.end(), [&](const LocalFixup& edge) {
                return edge.node == owner.Payload();
            });
    if (found == masm.local_fixups.end()) Reject(CompileError::InvalidProgram);
    const auto distance = static_cast<u32>(output.words.size()) - found->word;
    if (distance >= (1u << 18)) Reject(CompileError::CodeTooLarge);
    masm.Patch(found->word, masm.Here());
    masm.local_fixups.erase(found);
    // CompleteLocalFlags above belongs to the body path. Taken
    // edges land after it with their already-complete flags intact.
    // The skipped body did not necessarily establish any linear
    // PC/flags/location fact; subsequent observers read real state.
    static_pc_valid = alternate_pc_valid = false;
    terminal_condition = 18;
    nzcv_matches = 0;
    packed_current = false;
    shadow_dirty = Enabled();
    has_fault_location = false;
    deferred_test = {};
}

void Writer::HandleSelectFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    SnapshotCompareNZCV();
    const auto check = Read(cursor.Take(), 0);
    if (ctx.vector) {
        const auto yes = VectorValue(cursor.Take(), 0), no = VectorValue(cursor.Take(), 1);
        const auto mask = VectorTemporary();
        __ cmp(X(check), Operand(0));
        __ cset(v::w16, v::ne);
        __ neg(v::x16, Operand(v::x16));
        __ dup(Lanes(mask, 8), v::x16);
        __ bsl(Lanes(mask, 8), Lanes(yes, 8), Lanes(no, 8));
        VectorMove(destination, mask);
        RestoreCompareNZCV();
        return;
    }
    const auto native_width = width == 32 ? 32u : 64u;
    const auto yes = Read(cursor.Take(), 16, 64, false, native_width),
               no = Read(cursor.Take(), 17, 64, false, native_width);
    nzcv_matches = 0;
    __ cmp(X(check), Operand(0));
    __ csel(R(destination, native_width), R(yes, native_width), R(no, native_width), v::ne);
    RestoreCompareNZCV();
    if (width != 32) Narrow(destination, destination, width);
}

}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
