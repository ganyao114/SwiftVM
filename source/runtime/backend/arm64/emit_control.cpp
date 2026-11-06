#include "emit.h"

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned, bool) noexcept;
std::uintptr_t CpuidHelper() noexcept;
std::uintptr_t X87Helper() noexcept;
namespace detail {
void Writer::StackAdjust(bool subtract) {
    const auto spill_frame = output.frame_bytes - (Linked() ? 0 : 16);
    const u32 high = spill_frame >> 12, low = spill_frame & 4095;
    if (high) {
        if (subtract) masm.exact.sub(v::sp, v::sp, Operand(static_cast<u64>(high) << 12));
        else masm.exact.add(v::sp, v::sp, Operand(static_cast<u64>(high) << 12));
    }
    if (low) {
        if (subtract) masm.exact.sub(v::sp, v::sp, Operand(low));
        else masm.exact.add(v::sp, v::sp, Operand(low));
    }
}

void Writer::Branch(BlockId target) {
    if (target.value >= masm.labels.size()) Reject(CompileError::InvalidProgram);
    masm.fixups.push_back({static_cast<u32>(output.words.size()), target.value}); masm.exact.b(std::int64_t{0});
}

bool Writer::ShortBranch(Branch19 instruction, BlockId target) {
    // B.cond and CBZ/CBNZ share imm19. A forward edge borrows the original
    // fixup list; only a genuine final range miss asks for long emission.
    if (masm.labels[target.value] == UINT32_MAX) {
        if (long_forward_branches) return false;
        masm.fixups.push_back({static_cast<u32>(output.words.size()), target.value});
        instruction.Emit(masm.exact, 0); return true;
    }
    const auto distance = static_cast<std::int64_t>(masm.labels[target.value]) -
            static_cast<std::int64_t>(output.words.size());
    if (distance < -(std::int64_t{1} << 18) || distance >= (std::int64_t{1} << 18)) return false;
    instruction.Emit(masm.exact, distance);
    return true;
}

void Writer::TailJump(std::uintptr_t destination, bool link, u64 target_pc, u32 conditional_head) {
    if (!link && destination == options.dispatcher && dispatcher_literal != UINT32_MAX) {
        const auto distance = static_cast<std::int64_t>(dispatcher_literal) -
                static_cast<std::int64_t>(output.words.size());
        if (distance >= -(INT64_C(1) << 18) && distance < (INT64_C(1) << 18)) {
            masm.exact.ldr(v::x16, distance);
            masm.exact.br(v::x16); // LDR x16,immutable dispatcher DATA; BR x16.
            return;
        }
    }
    // CodeArena aligns entries. Keep DATA aligned without executing padding
    // before a resolved branch; the BR also skips padding on the far path.
    // A conditional head reaches this cold gate only when unresolved/far.
    // Its zero path already transfers elsewhere, so alignment here is cold.
    if (conditional_head != UINT32_MAX && (output.words.size() & 1)) masm.exact.nop();
    const auto branch = static_cast<u32>(output.words.size());
    const auto padding = branch & 1u;
    const auto pointer = branch + 2 + padding;
    masm.exact.ldr(v::x16, static_cast<std::int64_t>(2 + padding)); masm.exact.br(v::x16);
    if (padding) masm.exact.nop();
    masm.Data64(destination);
    if (!link && destination == options.dispatcher) dispatcher_literal = pointer;
    if (link) {
        output.links.push_back({LinkKind::Static, pointer * 4,
                (conditional_head == UINT32_MAX ? branch : conditional_head) * 4, (pointer + 2) * 4, target_pc});
        if (conditional_head != UINT32_MAX) {
            masm.Patch(conditional_head, branch);
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
    const auto count = [&](unsigned offset, bool decrement = false) {
        if (options.dispatch_mode != DispatchMode::InlineCounted) return;
        Memory(true, 64, 4, 28, offset / 8);
        if (decrement) masm.exact.sub(v::x4, v::x4, Operand(1)); else masm.exact.add(v::x4, v::x4, Operand(1)); // SUB/ADD x4,x4,#1, non-S.
        Memory(false, 64, 4, 28, offset / 8);
    };
    count(abi::ContextDispatchesOffset);
    u32 cache_pointer = UINT32_MAX;
    if (options.local_dispatch_cache) {
        const auto address = static_cast<u32>(output.words.size()); masm.exact.adr(v::x2, std::int64_t{0}); // ADR x2,local array.
        Extract(5, 0, 2, std::countr_zero(abi::DynamicCacheEntries));
        masm.exact.add(v::x2, v::x2, Operand(v::x5, v::LSL, 4)); // ADD x2,x2,x5,LSL #4: one sixteen-byte pair.
        masm.exact.ldp(v::x4, v::x16, MemOperand(v::x2)); // LDP x4,x16,[x2]: tag,entry.
        masm.exact.sub(v::x4, v::x4, Operand(v::x0)); // SUB x4,tag,x0, non-S.
        const auto mismatch = static_cast<u32>(output.words.size()); masm.exact.cbnz(v::x4, std::int64_t{0});
        count(abi::ContextHitsOffset);
        masm.exact.br(v::x16);
        if (output.words.size() & 1) masm.exact.nop(); // Unreachable DATA alignment.
        cache_pointer = static_cast<u32>(output.words.size());
        for (unsigned i = 0; i < abi::DynamicCacheWords; ++i) masm.Data32(0);
        const auto fallback = static_cast<u32>(output.words.size());
        // Empty/reset entries call this site's directory. A tag-match
        // fallback undoes only the preceding local hit before probing.
        count(abi::ContextHitsOffset, true);
        output.links.push_back({LinkKind::Dynamic, cache_pointer * 4, UINT32_MAX, fallback * 4, 0});
        masm.Patch(address, cache_pointer);
        masm.Patch(mismatch, masm.Here());
    }
    masm.exact.ldp(v::x2, v::x6, MemOperand(v::x28, abi::ContextSlotsOffset));
    u32 no_slots = UINT32_MAX;
    if (!options.callable_directory) { no_slots = static_cast<u32>(output.words.size()); masm.exact.cbz(v::x2, std::int64_t{0}); }
    masm.exact.eor(v::x5, v::x0, Operand(v::x0, v::LSR, abi::DispatchHashShift)); // EOR x5,x0,x0,LSR #hash.
    masm.exact.and_(v::x5, v::x5, Operand(v::x6));
    const auto probe = static_cast<u32>(output.words.size());
    count(abi::ContextProbesOffset);
    masm.exact.add(v::x3, v::x2, Operand(v::x5, v::LSL, abi::DispatchSlotShift));
    masm.exact.ldp(v::x4, v::x16, MemOperand(v::x3));
    masm.exact.sub(v::x4, v::x4, Operand(v::x0)); // SUB x4,tag,x0, non-S.
    const auto mismatch = static_cast<u32>(output.words.size()); masm.exact.cbnz(v::x4, std::int64_t{0});
    u32 pending = UINT32_MAX;
    if (!options.callable_directory) { pending = static_cast<u32>(output.words.size()); masm.exact.cbz(v::x16, std::int64_t{0}); }
    count(abi::ContextHitsOffset);
    u32 not_ready = UINT32_MAX, no_alias = UINT32_MAX;
    if (cache_pointer != UINT32_MAX) {
        masm.exact.ldr(v::w4, MemOperand(v::x3, 16)); // LDR w4,[x3,#16]: actual ready code_id, not callable pending payload.
        masm.exact.add(v::w4, v::w4, Operand(1)); // ADD w4,w4,#1, non-S: UINT32_MAX becomes zero.
        not_ready = static_cast<u32>(output.words.size()); masm.exact.cbz(v::w4, std::int64_t{0});
        const auto alias = static_cast<u32>(output.words.size());
        masm.exact.ldr(v::x2, static_cast<std::int64_t>(cache_pointer + abi::DynamicCacheWords - 2) - alias); // LDR x2,RW-base.
        no_alias = static_cast<u32>(output.words.size()); masm.exact.cbz(v::x2, std::int64_t{0});
        Extract(5, 0, 2, std::countr_zero(abi::DynamicCacheEntries));
        masm.exact.add(v::x2, v::x2, Operand(v::x5, v::LSL, 4)); // ADD x2,x2,x5,LSL #4.
        masm.exact.stp(v::x0, v::x16, MemOperand(v::x2)); // STP x0,x16,[x2]: replace only this ready target's selected pair.
    }
    for (const auto edge : {not_ready, no_alias})
        if (edge != UINT32_MAX) masm.Patch(edge, masm.Here());
    masm.exact.br(v::x16); // This source site's BR x16.
    masm.Patch(mismatch, masm.Here());
    masm.exact.ldrb(v::w4, MemOperand(v::x3, abi::DispatchOccupiedOffset));
    const auto empty = static_cast<u32>(output.words.size()); masm.exact.cbz(v::w4, std::int64_t{0});
    masm.exact.add(v::x5, v::x5, Operand(1)); // ADD x5,x5,#1.
    masm.exact.and_(v::x5, v::x5, Operand(v::x6));
    const auto distance = static_cast<std::int64_t>(probe) - masm.Here();
    masm.exact.b(distance);
    for (const auto edge : {no_slots, pending, empty})
        if (edge != UINT32_MAX) masm.Patch(edge, masm.Here());
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
    else { masm.exact.ldp(v::x19, v::x30, MemOperand(v::sp, 16, v::PostIndex)); masm.exact.ret(v::x30); }
}

unsigned Writer::PrepareTerminalCondition(unsigned condition, bool& restore, bool& flip) {
    if (condition == 16 || condition == 17) {
        SnapshotCompareNZCV();
        nzcv_matches = 0;
        if (RawAF()) {
            masm.exact.eor(v::x16, v::x29, Operand(v::x1));
            masm.exact.tst(v::x16, Operand(16)); // TST x16,#16: canonical AF is the xor's bit4.
        } else masm.exact.tst(v::x1, Operand(u64{1} << 26)); // TST x1,#(1<<26).
        condition = condition == 16 ? 1u : 0u;
        restore = NativeNZCV();
    } else if (NativeNZCV()) {
        if (condition == 20 || condition == 21) condition = 9u ^ (condition & 1u); // Guest C|Z -> native LS.
        else if (condition == 2 || condition == 3) condition ^= 1u;
        else if (condition == 8 || condition == 9) { masm.exact.cfinv(); flip = true; }
    } else EnsureNZCV(condition);
    return condition;
}

void Writer::RestoreTerminalCondition(bool restore, bool flip) {
    if (flip) masm.exact.cfinv();
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
        masm.exact.b(std::int64_t{0}, static_cast<v::Condition>(condition ^ (primary_yes ? 0u : 1u)));
        PublishPCAndJump(other);
        TailJump(options.dispatcher, true, primary, head);
        return;
    }
    const auto skip = static_cast<u32>(output.words.size()); masm.exact.b(std::int64_t{0}, static_cast<v::Condition>(condition ^ 1u));
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
        const auto at = static_cast<u32>(output.words.size()); masm.exact.b(std::int64_t{0}, static_cast<v::Condition>(skip));
        RestoreTerminalCondition(restore, flip);
        Branch(true_falls_through ? terminal.second : terminal.first);
        masm.Patch(at, masm.Here());
        RestoreTerminalCondition(restore, flip);
    } else if (!ShortBranch(Branch19::Flags(skip ^ 1u),
                           true_falls_through ? terminal.second : terminal.first)) {
        masm.exact.b(std::int64_t{2}, static_cast<v::Condition>(skip)); // B.cond +8: only the actual long edge uses B26.
        Branch(true_falls_through ? terminal.second : terminal.first);
    }
    if (!true_falls_through && terminal.second != next) Branch(terminal.second);
}

void Writer::Terminal(const StoredTerminal& terminal, BlockId next) {
    CompleteLocalFlags();
    switch (terminal.kind) {
        case TerminalKind::Return: return HandleReturn(terminal);
        case TerminalKind::Dispatch: return HandleDispatch();
        case TerminalKind::Jump: return HandleJump(terminal, next);
        case TerminalKind::Branch: return HandleBranch(terminal, next);
        case TerminalKind::Unreachable: return HandleUnreachable();
        default: Reject(CompileError::UnsupportedControlFlow);
    }
}

void Writer::HandleReturn(const StoredTerminal& terminal) {
    do {
        Shadow(false);
            if (terminal.HasValue()) {
                const auto value = RawValue(terminal.value.value, 16);
                const auto type = terminal.value_type == Type::Void ? source.Operations()[terminal.value.value].type : terminal.value_type;
                Narrow(0, value, Width(type));
            } else Immediate(0, 0);
            Epilogue(); break;
    } while (false);
}

void Writer::HandleDispatch() {
    do {
        if (!options.private_uniforms) Shadow(false);
            if (Linked()) {
                StackAdjust(false);
                if (static_pc_valid && alternate_pc_valid) {
                    // The selected PC is already committed. Use its two
                    // real constant edges without extending the dead SSA
                    // condition's lifetime or introducing a second cache.
                    SnapshotCompareNZCV();
                    PCMemory(true); Immediate(17, static_pc);
                    masm.exact.cmp(v::x0, Operand(v::x17));
                    const auto skip = static_cast<u32>(output.words.size()); masm.exact.b(std::int64_t{0}, v::ne);
                    RestoreCompareNZCV();
                    TailJump(options.dispatcher, true, static_pc);
                    masm.Patch(skip, masm.Here());
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
    } while (false);
}

void Writer::HandleJump(const StoredTerminal& terminal, BlockId next) {
    do {
        if (terminal.first != next) Branch(terminal.first); break;
    } while (false);
}

void Writer::HandleBranch(const StoredTerminal& terminal, BlockId next) {
    do {
        {
            const auto& home = allocation.homes[terminal.value.value];
            const auto type = terminal.value_type == Type::Void ? source.Operations()[terminal.value.value].type : terminal.value_type;
            if (deferred_test.bits) {
                Width(type);
                const auto& deferred = deferred_test;
                const auto cbnz = Branch19::Zero(R(deferred.value_register, deferred.bits == 64 ? 64 : 32), true);
                if (!ShortBranch(cbnz, terminal.first)) {
                    cbnz.Inverse().Emit(masm.exact, 2);
                    Branch(terminal.first);
                }
                // Only the zero exit completes TEST. Its current value,
                // not the nonzero entry snapshot, is known to be zero.
                Immediate(1, 0); Immediate(29, 0);
                masm.exact.cmp(v::x1, Operand(0)); // CMP x1,#0: N0 Z1 !guestC1 V0.
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
            if (!ShortBranch(Branch19::Zero(v::x16, !true_falls_through),
                             true_falls_through ? terminal.second : terminal.first)) {
                Branch19::Zero(v::x16, true_falls_through).Emit(masm.exact, 2); // CBNZ/CBZ x16,+8
                Branch(true_falls_through ? terminal.second : terminal.first);
            }
            if (!true_falls_through && terminal.second != next) Branch(terminal.second);
            break;
        }
    } while (false);
}

void Writer::HandleUnreachable() {
    do {
        Shadow(false); masm.exact.brk(0); break;
    } while (false);
}

void Writer::HandleCondSetFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    do {
{
            RequireFlags(); const auto condition = Condition(cursor.Take());
            if (home.terminal_only) { terminal_condition = condition; break; }
            if (condition >= 14) Immediate(destination, 1);
            else {
                EnsureNZCV(condition);
                const bool flip = NativeNZCV() && (condition == 8 || condition == 9);
                const auto host = NativeNZCV() && (condition == 2 || condition == 3) ? condition ^ 1 : condition;
                if (flip) masm.exact.cfinv();
                masm.exact.cset(X(destination), static_cast<v::Condition>(host));
                if (flip) masm.exact.cfinv();
            }
            break;
        }
    } while (false);
}

void Writer::HandleCondSelectFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
{
            RequireFlags(); const auto condition = Condition(cursor.Take());
            const auto native_width = width == 32 ? 32u : 64u;
            const auto yes = Read(cursor.Take(), 16, 64, false, native_width),
                       no = Read(cursor.Take(), 17, 64, false, native_width);
            if (condition >= 14) { Narrow(destination, yes, width); break; }
            EnsureNZCV(condition);
            const bool flip = NativeNZCV() && (condition == 8 || condition == 9);
            const auto host = NativeNZCV() && (condition == 2 || condition == 3) ? condition ^ 1 : condition;
            if (flip) masm.exact.cfinv();
            masm.exact.csel(R(destination, native_width), R(yes, native_width), R(no, native_width), static_cast<v::Condition>(host));
            if (flip) masm.exact.cfinv();
            if (width != 32) Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

void Writer::HandleGotoFamily(OpContext& ctx) {
    const auto& operation = ctx.operation;
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    const auto save_result = [&] { SaveResult(ctx); };
    do {
{
            const auto input = cursor.Take();
            auto check = Read(input, 0);
            // The returned numeric value is available on both paths. Keep
            // the full input predicate even if a narrower result aliases it.
            if (destination == check && width < Width(View(input))) { Move(0, check); check = 0; }
            Narrow(destination, check, width);
            save_result();
            masm.local_fixups.push_back({node.value, static_cast<u32>(output.words.size())});
            Branch19::Zero(X(check), operation.opcode == OpCode::Goto).Emit(masm.exact, 0);
            break;
        }
    } while (false);
}

void Writer::HandleBindLabelFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    do {
{
            const auto owner = cursor.Take();
            if (owner.Kind() != OperandKind::Value || owner.Payload() >= node.value)
                Reject(CompileError::UnsupportedOperand);
            const auto found = std::find_if(masm.local_fixups.begin(), masm.local_fixups.end(),
                    [&](const LocalFixup& edge) { return edge.node == owner.Payload(); });
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
            nzcv_matches = 0; packed_current = false; shadow_dirty = Enabled();
            has_fault_location = false;
            deferred_test = {};
            break;
        }
    } while (false);
}

void Writer::HandleSelectFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto width = ctx.width;
    do {
{
            SnapshotCompareNZCV();
            const auto check = Read(cursor.Take(), 0);
            const auto native_width = width == 32 ? 32u : 64u;
            const auto yes = Read(cursor.Take(), 16, 64, false, native_width),
                    no = Read(cursor.Take(), 17, 64, false, native_width);
            nzcv_matches = 0;
            masm.exact.cmp(X(check), Operand(0));
            masm.exact.csel(R(destination, native_width), R(yes, native_width), R(no, native_width), v::ne);
            RestoreCompareNZCV();
            if (width != 32) Narrow(destination, destination, width);
            break;
        }
    } while (false);
}

} // namespace detail
} // namespace swift::runtime::backend::arm64
