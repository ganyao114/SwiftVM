#include "emit.h"

#include "runtime/arch/arm64/numeric.h"
#include "runtime/ir/flag_effects.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace swift::runtime::backend::x64::detail {
Writer::Writer(const Program& s, const Allocation& a, CompileResult& o, CompileOptions c)
        : CodeGenerator(0, DontSetProtectRWE, nullptr, &o.words)
        , source(s)
        , allocation(a)
        , output(o)
        , options(c)
        , guest_pc(c.entry_pc)
        , blocks(s.Blocks().size())
        , locals(s.Operations().size()) {
    scalar_residents.fill(NodeId::invalid);
    vector_residents.fill(NodeId::invalid);
    cold_fp.reserve(a.cold_fp_count);
    cold_numeric.reserve(a.cold_fp_count);
    fp_stubs.reserve(a.cold_fp_count);
    if (a.cold_fp_count) fp_stub_index.reserve(a.cold_fp_count);
    cold_events.reserve(a.cold_event_count);
    frame_bytes = a.spill_bytes + (Linked() ? 0 : 24);
    mxcsr_slot = a.spill_bytes;
    output.frame_bytes = frame_bytes + (Linked() ? 0 : 48);
    output.spill_slots = a.spill_bytes / 8;
}

void Writer::Reject(CompileError e) { throw Rejected{e, current, block}; }

void Writer::PublishFlags() {
    if (options.local_flags && flags.dirty) {
        push(rdx);
        push(rsi);
        push(r10);
        push(r11);
        flags_stack_bias = 32;
        MaterializeFlags();
        flags_stack_bias = 0;
        pop(r11);
        pop(r10);
        pop(rsi);
        pop(rdx);
    }
    if (options.flags_offset != UINT32_MAX) mov(qword[r12 + options.flags_offset], rbx);
}

int Writer::PinnedSlot(std::uint64_t offset, unsigned bytes) const {
    if (!Pinned() || options.pinned_uniform_offset == UINT32_MAX || !bytes) return -1;
    const auto begin = options.pinned_uniform_offset;
    if (offset < begin) return -1;
    const auto relative = offset - begin;
    if (relative >= std::uint64_t{options.pinned_uniform_slots} * 8 ||
        relative + bytes > std::uint64_t{options.pinned_uniform_slots} * 8)
        return -1;
    return static_cast<int>(relative / 8);
}

void Writer::PublishPinned() {
    if (!Pinned()) return;
    for (unsigned i = 0; i < options.pinned_uniform_slots; ++i)
        mov(qword[r12 + options.pinned_uniform_offset + i * 8], Reg64(PinnedRegister(i)));
}

void Writer::ReloadPinned() {
    if (!Pinned()) return;
    for (unsigned i = 0; i < options.pinned_uniform_slots; ++i)
        mov(Reg64(PinnedRegister(i)), qword[r12 + options.pinned_uniform_offset + i * 8]);
}

void Writer::Prologue() {
    if (!Linked()) {
        push(rbx);
        push(rbp);
        push(r12);
        push(r13);
        push(r14);
        push(r15);
        mov(r12, rdi);
    }
    if (frame_bytes) sub(rsp, frame_bytes);
    if (!Linked()) {
        stmxcsr(dword[rsp + mxcsr_slot]);
        if (ArmNativeFP()) {
            constexpr unsigned rounding[]{0, 2, 1, 3};
            const auto control = options.arm_fpcr == UINT32_MAX ? 0u : options.arm_fpcr;
            mov(dword[rsp + mxcsr_slot + 4],
                0x1f80 | (rounding[(control >> 22) & 3] << 13) |
                        ((control & (1u << 24)) ? 0x8040 : 0));
            ldmxcsr(dword[rsp + mxcsr_slot + 4]);
        } else if (options.native_fp_environment && options.mxcsr_offset != UINT32_MAX) {
            mov(eax, dword[r12 + options.mxcsr_offset]);
            and_(eax, 0xe040);
            or_(eax, 0x1f80);
            mov(dword[rsp + mxcsr_slot + 4], eax);
            ldmxcsr(dword[rsp + mxcsr_slot + 4]);
        }
        if (options.flags_offset != UINT32_MAX)
            mov(rbx, qword[r12 + options.flags_offset]);
        else
            xor_(ebx, ebx);
    }
}

void Writer::Restore() {
    if (!Linked()) {
        if (ArmNativeFP()) {
            HarvestArmFPStatus(false, false);
        } else if (options.native_fp_environment && options.mxcsr_offset != UINT32_MAX) {
            stmxcsr(dword[rsp + mxcsr_slot + 4]);
            mov(r11d, dword[rsp + mxcsr_slot + 4]);
            and_(r11d, 63);
            or_(dword[r12 + options.mxcsr_offset], r11d);
        }
        ldmxcsr(dword[rsp + mxcsr_slot]);
    }
    if (frame_bytes) add(rsp, frame_bytes);
    if (!Linked()) {
        pop(r15);
        pop(r14);
        pop(r13);
        pop(r12);
        pop(rbp);
        pop(rbx);
    }
}

void Writer::Exit(u64 reason) {
    const auto previous_flags = flags;
    PublishFlags();
    const auto kind = static_cast<NativeExit>(reason & 0xff);
    if (reason && kind != NativeExit::GuestException && kind != NativeExit::GuestEvents &&
        kind != NativeExit::SemanticException && options.pc_offset != UINT32_MAX) {
        mov(rax, guest_pc);
        mov(qword[r12 + options.pc_offset], rax);
    }
    Restore();
    mov(rax, reason);
    if (Linked()) {
        HostAddress(r11, options.exit_handler, HostSymbol::Exit);
        jmp(r11);
    } else {
        ret();
    }
    flags = previous_flags;
}

void Writer::StaticExit(u64 pc) {
    const auto previous_flags = flags;
    ExportLinkedFlags();
    mov(rdi, pc);
    if (options.pc_offset != UINT32_MAX) mov(qword[r12 + options.pc_offset], rdi);
    Restore();
    if (!Linked()) {
        xor_(eax, eax);
        ret();
        flags = previous_flags;
        return;
    }
    align(8);
    backend::LinkSite site;
    site.target_pc = pc;
    site.branch_offset = getSize();
    Label data;
    jmp(ptr[rip + data]);
    nop(2);
    L(data);
    site.data_offset = getSize();
    dq(0);
    dq(0);
    site.fallback_offset = getSize();
    HostAddress(r11, options.dispatcher, HostSymbol::Dispatcher);
    jmp(r11);
    output.links.push_back(site);
    flags = previous_flags;
}

void Writer::LoadTerminalValue(const StoredTerminal& t, bool mask_rematerialized) {
    const auto& operation = source.Operations()[t.value.value];
    const auto& home = allocation.homes[t.value.value];
    if (home.spilled && home.uniform_offset != UINT32_MAX) {
        Load(rax, r12, home.uniform_offset, TypeBits(operation.type));
    } else if (home.rematerialize) {
        mov(rax, source.Operands(t.value).front().Payload());
        if (mask_rematerialized) Mask(rax, TypeBits(operation.type));
    } else if (home.spilled) {
        mov(rax, qword[rsp + home.stack_offset]);
    } else {
        mov(rax, Reg64(home.reg));
    }
    const auto type = t.value_type == ir::Type::Void ? operation.type : t.value_type;
    Mask(rax, TypeBits(type));
}

void Writer::EmitTerminalZeroOrBitBranch(const StoredTerminal& t,
                                         OpCode op,
                                         CellView first,
                                         CellRange::Iterator input,
                                         bool needs_flags) {
    const bool zero = op != OpCode::TestBit;
    if (zero && flags.native && TypeBits(View(first)) >= 32) {
        Read(first, rcx);
        Label is_zero;
        jrcxz(is_zero);
        const auto previous = flags;
        BranchTo(op == OpCode::TestZero ? t.second : t.first);
        L(is_zero);
        flags = previous;
        BranchTo(op == OpCode::TestZero ? t.first : t.second, true);
        return;
    }
    if (needs_flags) SaveNativeFlags(true);
    flags.native = false;
    if (zero) {
        const auto bits = std::min(64u, TypeBits(View(first)));
        Reg64 src = rax;
        if ((bits == 32 || bits == 64) && ResidentScalar(first, bits, src)) {
            PadFor32ByteBoundary(9u);
            test(R(src, bits), R(src, bits));
        } else {
            Read(first, rax);
            PadFor32ByteBoundary(9u);
            test(rax, rax);
        }
        ConditionalBranch(op == OpCode::TestZero ? 4 : 5, t.first, t.second);
        return;
    }
    const auto bit = (*input).Payload();
    if (bit >= TypeBits(View(first))) {
        BranchTo(t.second, true);
    } else {
        Read(first, rax);
        bt(rax, bit);
        ConditionalBranch(2, t.first, t.second);
    }
}

void Writer::EmitTerminalConditionBranch(const StoredTerminal& t,
                                         ir::Condition condition,
                                         bool needs_flags) {
    if (condition_saved) {
        if (needs_flags) SaveNativeFlags(true);
        flags.native = false;
        PadFor32ByteBoundary((allocation.condition_slot < 128 ? 5u : 8u) + 6u);
        cmp(byte[rsp + allocation.condition_slot], 0);
        ConditionalBranch(5, t.first, t.second);
        return;
    }
    if (options.local_flags && flags.dirty) {
        const auto cc = NativeCondition(condition);
        if (cc == 16)
            BranchTo(t.first, true);
        else
            ConditionalBranch(cc, t.first, t.second);
        return;
    }
    if (needs_flags) SaveNativeFlags(true);
    flags.native = false;
    PackedCondition(rax, condition);
    test(rax, rax);
    ConditionalBranch(5, t.first, t.second);
}

void Writer::EmitTerminalFlagMaskBranch(const StoredTerminal& t,
                                        OpCode op,
                                        u64 mask_payload,
                                        bool needs_flags) {
    MaterializeFlags();
    if (needs_flags) SaveNativeFlags(true);
    flags.native = false;
    const auto mask = PackedFlagMask(mask_payload);
    if (mask <= UINT32_MAX) {
        test(ebx, static_cast<u32>(mask));
    } else {
        mov(rax, mask);
        test(rbx, rax);
    }
    ConditionalBranch(op == OpCode::TestNotFlags ? 4 : 5, t.first, t.second);
}

void Writer::EmitBranchTerminal(const StoredTerminal& t) {
    const auto& home = allocation.homes[t.value.value];
    const auto& operation = source.Operations()[t.value.value];
    const bool needs_flags = options.local_flags && (allocation.flags_input[t.first.value] ||
                                                     allocation.flags_input[t.second.value]);
    if (home.terminal_only) {
        auto input = source.Operands(t.value).begin();
        const auto first = *input++;
        const auto op = operation.opcode;
        if (op == OpCode::TestZero || op == OpCode::TestNotZero || op == OpCode::TestBit) {
            EmitTerminalZeroOrBitBranch(t, op, first, input, needs_flags);
            return;
        }
        std::optional<ir::Condition> condition;
        if (op == OpCode::CondSet || op == OpCode::LocalCondSet)
            condition = static_cast<ir::Condition>(first.Payload());
        else
            condition = ir::FlagCondition(first.Payload(), op == OpCode::TestNotFlags);
        if (condition) {
            EmitTerminalConditionBranch(t, *condition, needs_flags);
            return;
        }
        EmitTerminalFlagMaskBranch(t, op, first.Payload(), needs_flags);
        return;
    }
    if (needs_flags) SaveNativeFlags(true);
    flags.native = false;
    LoadTerminalValue(t, false);
    test(rax, rax);
    ConditionalBranch(5, t.first, t.second);
}

void Writer::EmitReturnTerminal(const StoredTerminal& t) {
    MaterializeFlags();
    if (t.HasValue()) {
        if (source.Operations()[t.value.value].type == ir::Type::V128)
            Reject(CompileError::UnsupportedType);
        LoadTerminalValue(t, true);
    } else {
        xor_(eax, eax);
    }
    PublishFlags();
    Restore();
    if (Linked()) {
        HostAddress(r11, options.exit_handler, HostSymbol::Exit);
        jmp(r11);
    } else {
        ret();
    }
}

void Writer::EmitDispatchTerminal() {
    if (static_pc_known) {
        StaticExit(static_pc);
        return;
    }
    ExportLinkedFlags();
    if (options.pc_offset != UINT32_MAX)
        mov(rdi, qword[r12 + options.pc_offset]);
    else
        xor_(edi, edi);
    Restore();
    if (Linked()) {
        output.has_indirect_dispatch = true;
        HostAddress(r11, options.dispatcher, HostSymbol::Dispatcher);
        jmp(r11);
    } else {
        xor_(eax, eax);
        ret();
    }
}

void Writer::Terminal(const StoredTerminal& t) {
    EndArmFPRegion();
    switch (t.kind) {
        case TerminalKind::Jump:
            BranchTo(t.first, true);
            return;
        case TerminalKind::Branch:
            EmitBranchTerminal(t);
            return;
        case TerminalKind::Return:
            EmitReturnTerminal(t);
            return;
        case TerminalKind::Dispatch:
            EmitDispatchTerminal();
            return;
        default:
            Reject(CompileError::UnsupportedControlFlow);
    }
}

void Writer::WriteUniformBackedResult(OpContext& c, const Home& h) {
    if (!h.uniform_store_bytes) return;
    if (c.vector_result) {
        if (options.avx)
            vmovdqu(ptr[r12 + h.uniform_offset], c.vector);
        else
            movdqu(ptr[r12 + h.uniform_offset], c.vector);
        return;
    }
    if (!c.scalar_normalized) Mask(c.destination, std::min(64u, c.width));
    Store(c.destination, r12, h.uniform_offset, h.uniform_store_bytes * 8);
}

void Writer::WriteResult(OpContext& c) {
    if (c.saved || c.operation.type == ir::Type::Void) return;
    const auto& h = allocation.homes[c.node.value];
    if (h.terminal_only) return;
    if (c.vector_result && c.vector.getIdx() == 0) {
        xmm0_node = c.node.value;
        kept_xmm0 = true;
    }
    if (h.spilled && h.uniform_offset != UINT32_MAX) {
        WriteUniformBackedResult(c, h);
        return;
    }
    if (c.vector_result) {
        if (h.spilled) {
            if (options.avx)
                vmovdqu(ptr[rsp + h.stack_offset], c.vector);
            else
                movdqu(ptr[rsp + h.stack_offset], c.vector);
        } else if (c.vector.getIdx() != h.reg) {
            if (options.avx)
                vmovdqa(Xmm(h.reg), c.vector);
            else
                movdqa(Xmm(h.reg), c.vector);
        }
        if (!h.spilled) vector_residents[h.reg] = c.node.value;
        return;
    }
    if (!c.scalar_normalized && c.width == 32 && !h.spilled) {
        mov(Reg32(h.reg), c.destination.cvt32());
    } else {
        if (!c.scalar_normalized) Mask(c.destination, std::min(64u, c.width));
        if (h.spilled)
            mov(qword[rsp + h.stack_offset], c.destination);
        else if (c.destination.getIdx() != h.reg)
            mov(Reg64(h.reg), c.destination);
    }
    if (!h.spilled) scalar_residents[h.reg] = c.node.value;
}

void Writer::Dispatch(OpContext& c) {
    switch (c.operation.opcode) {
#define INST(name, ...)                                                                            \
    case OpCode::name:                                                                             \
        Handle(Op<OpCode::name>{}, c);                                                             \
        break;
#include "runtime/ir/opcodes.inc"
#undef INST
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
}

bool Writer::PreservesScratchFpCaches(OpCode opcode) const {
    if (IsPureScalarAluOrImmOp(opcode)) return true;
    switch (opcode) {
        case OpCode::Nop:
        case OpCode::SetLocation:
        case OpCode::AdvancePC:
        case OpCode::InvertCarry:
        case OpCode::SetCarry:
        case OpCode::ClearFlags:
        case OpCode::LoadUniform:
        case OpCode::StoreUniform:
        case OpCode::LoadMemory:
        case OpCode::LoadMemoryTSO:
        case OpCode::StoreMemory:
        case OpCode::StoreMemoryTSO:
        case OpCode::VecSharedZero:
        case OpCode::VecLoadConst:
        case OpCode::BitCast:
        case OpCode::GetResult:
        case OpCode::VecDup64:
        case OpCode::GuestSafepoint:
        case OpCode::GuestDispatchSafepoint:
            return true;
        case OpCode::VecExtract64:
            return options.avx || options.sse41;
        default:
            return false;
    }
}

void Writer::ValidateCompileOptions() {
    if (options.arm_fpcr != UINT32_MAX && !runtime::arm64::IsSupportedFPCR(options.arm_fpcr))
        Reject(CompileError::UnsupportedOperand);
    if (!Pinned()) return;
    if (!Linked() || options.pinned_uniform_slots != 2 ||
        options.pinned_uniform_offset == UINT32_MAX ||
        options.pinned_uniform_offset > UINT32_MAX - (options.pinned_uniform_slots * 8u - 1))
        Reject(CompileError::UnsupportedOperand);
    const auto bytes = options.pinned_uniform_slots * 8u;
    const auto overlaps = [&](std::uint32_t field) {
        return field != UINT32_MAX &&
               std::uint64_t{field} < std::uint64_t{options.pinned_uniform_offset} + bytes &&
               options.pinned_uniform_offset < std::uint64_t{field} + 8;
    };
    if (overlaps(options.pc_offset) || overlaps(options.flags_offset) ||
        overlaps(options.mxcsr_offset))
        Reject(CompileError::UnsupportedOperand);
}

void Writer::EmitUnitEntry(u32 block_id, u64 pc, bool fp_guard) {
    flags = {};
    Prologue();
    if (options.local_flags && (allocation.flags_input[block_id] || fp_guard)) ImportLinkedFlags();
    if (fp_guard) {
        Label ready;
        cmp(dword[r12 + options.arm_fpcr_offset], options.arm_fpcr);
        je(ready, T_NEAR);
        guest_pc = pc;
        Exit(runtime::ExitValue(runtime::NativeExit::FPEnvironment));
        L(ready);
    }
    PrepareEdge(BlockId{block_id});
}

void Writer::ResetBlockState(u32 b) {
    block = BlockId{b};
    L(blocks[b]);
    static_pc_known = false;
    has_fault_location = false;
    terminated = false;
    condition_saved = false;
    xmm15_fp_const = -1;
    xmm0_node = NodeId::invalid;
    xmm1_bcast_node = NodeId::invalid;
    flags = {options.local_flags && allocation.flags_input[b] != 0,
             options.local_flags && allocation.native_entry[b],
             options.local_flags && allocation.flags_input[b] != 0 && !allocation.native_entry[b],
             options.local_flags && allocation.native_entry[b],
             true};
}

void Writer::UpdateScratchCaches(const OpContext& c, bool preserve_fp_caches) {
    const auto def_pos = allocation.homes[c.node.value].definition_position;
    if (!kept_xmm15_fp_const && !preserve_fp_caches) xmm15_fp_const = -1;
    if (!kept_xmm1_bcast && !preserve_fp_caches) xmm1_bcast_node = NodeId::invalid;
    if (xmm1_bcast_node != NodeId::invalid &&
        allocation.homes[xmm1_bcast_node].last_use <= def_pos)
        xmm1_bcast_node = NodeId::invalid;
    if (!kept_xmm0 || (xmm0_node != NodeId::invalid &&
                       allocation.homes[xmm0_node].last_use <= def_pos))
        xmm0_node = NodeId::invalid;
}

void Writer::EmitNode(u32 i, u32 b, ExtraEntryList& extra_entries) {
    current = NodeId{i};
    const auto& op = source.Operations()[i];
    if (op.opcode == OpCode::FaultLocation) {
        guest_pc = source.Operands(current).front().Payload();
        has_fault_location = true;
        return;
    }
    if (op.opcode == OpCode::EntryPoint) {
        if (!Linked()) Reject(CompileError::UnsupportedControlFlow);
        const auto entry_pc = source.Operands(current).front().Payload();
        if (entry_pc == options.entry_pc) return;
        xmm15_fp_const = -1;
        xmm0_node = NodeId::invalid;
        xmm1_bcast_node = NodeId::invalid;
        output.entries.push_back({entry_pc, 0});
        extra_entries.push_back({unsigned(output.entries.size() - 1), b});
        return;
    }
    OpContext c{op, current, source.Operands(current)};
    for (auto cell : c.operands) {
        if (c.count < c.args.size()) c.args[c.count] = cell;
        ++c.count;
    }
    c.width = TypeBits(op.type);
    c.vector_result = op.type == ir::Type::V128;
    c.destination = rax;
    c.vector = xmm0;
    fault_committed_flags = false;
    kept_xmm15_fp_const = false;
    kept_xmm1_bcast = false;
    kept_xmm0 = PreservesCachedXmm0(op.opcode, c.vector_result);
    if (allocation.homes[i].terminal_only) return;
    const bool preserve_fp_caches = PreservesScratchFpCaches(op.opcode);
    PrepareArmFPRegion(c);
    PrepareFlags(c);
    Dispatch(c);
    WriteResult(c);
    UpdateScratchCaches(c, preserve_fp_caches);
    if (op.flag_outputs && !SupportsFlagOutputs(op.opcode))
        Reject(CompileError::UnsupportedFlags);
}

void Writer::EmitBlock(u32 b, ExtraEntryList& extra_entries) {
    ResetBlockState(b);
    const auto& body = source.Blocks()[b];
    for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i)
        EmitNode(i, b, extra_entries);
    if (!terminated) Terminal(body.terminal);
}

void Writer::Run() {
    ValidateCompileOptions();
    const bool fp_guard = allocation.uses_fpcr && options.arm_fpcr != UINT32_MAX &&
                          options.arm_fpcr_offset != UINT32_MAX;
    EmitUnitEntry(source.EntryBlock().value, options.entry_pc, fp_guard);
    ExtraEntryList extra_entries;
    for (std::size_t order_idx = 0; order_idx < allocation.block_order.size(); ++order_idx) {
        const auto b = allocation.block_order[order_idx];
        fallthrough_block = order_idx + 1 < allocation.block_order.size()
                                    ? allocation.block_order[order_idx + 1]
                                    : BlockId::invalid;
        EmitBlock(b, extra_entries);
    }
    fallthrough_block = BlockId::invalid;
    xmm15_fp_const = -1;
    xmm0_node = NodeId::invalid;
    xmm1_bcast_node = NodeId::invalid;
    EmitColdPaths();
    for (auto [index, b] : extra_entries) {
        align(16);
        output.entries[index].offset = getSize();
        EmitUnitEntry(b, output.entries[index].pc, fp_guard);
        jmp(blocks[b], T_NEAR);
    }
    EmitFPConstants();
    while (getSize() & 3) db(0x90);
    ready(PROTECT_RW);
}
}  // namespace swift::runtime::backend::x64::detail
