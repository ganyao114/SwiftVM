#include <algorithm>
#include <cstring>
#include "emit.h"
#include "runtime/arch/arm64/numeric.h"

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
        if (options.native_fp_environment && options.mxcsr_offset != UINT32_MAX) {
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
        if (options.native_fp_environment && options.mxcsr_offset != UINT32_MAX) {
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
    } else
        ret();
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

void Writer::Terminal(const StoredTerminal& t) {
    EndArmFPRegion();
    if (t.kind == TerminalKind::Jump) {
        BranchTo(t.first);
        return;
    }
    if (t.kind == TerminalKind::Branch) {
        const auto& h = allocation.homes[t.value.value];
        if (h.terminal_only) {
            const auto& operation = source.Operations()[t.value.value];
            if (options.local_flags) {
                const auto mask = source.Operands(t.value).front().Payload();
                ir::Condition condition{};
                bool direct = true;
                if (operation.opcode == OpCode::CondSet || operation.opcode == OpCode::LocalCondSet)
                    condition = static_cast<ir::Condition>(mask);
                else {
                    switch (mask) {
                        case 1:
                            condition = ir::Condition::CS;
                            break;
                        case 2:
                            condition = ir::Condition::VS;
                            break;
                        case 4:
                            condition = ir::Condition::EQ;
                            break;
                        case 8:
                            condition = ir::Condition::MI;
                            break;
                        default:
                            direct = false;
                            break;
                    }
                }
                if (direct) {
                    const bool yes_needs = allocation.flags_input[t.first.value] != 0;
                    const bool no_needs = allocation.flags_input[t.second.value] != 0;
                    if (yes_needs && no_needs) SaveNativeFlags(true);
                    auto cc = NativeCondition(condition);
                    if (operation.opcode == OpCode::TestNotFlags) cc ^= 1;
                    if (cc == 16) {
                        BranchTo(t.first);
                        return;
                    }
                    if (!yes_needs || no_needs) {
                        NativeJump(cc, blocks[t.first.value]);
                        BranchTo(t.second);
                    } else {
                        NativeJump(cc ^ 1, blocks[t.second.value]);
                        BranchTo(t.first);
                    }
                    return;
                }
                MaterializeFlags();
                SaveNativeFlags(true);
            }
            const auto mask = PackedFlagMask(source.Operands(t.value).front().Payload());
            if (mask <= UINT32_MAX)
                test(ebx, static_cast<u32>(mask));
            else {
                mov(rax, mask);
                test(rbx, rax);
            }
            if (operation.opcode == OpCode::TestNotFlags)
                jz(blocks[t.first.value], T_NEAR);
            else
                jnz(blocks[t.first.value], T_NEAR);
            jmp(blocks[t.second.value], T_NEAR);
            return;
        }
        if (options.local_flags) {
            SaveNativeFlags(true);
            flags.native = false;
        }
        if (h.spilled)
            mov(rax, qword[rsp + h.stack_offset]);
        else
            mov(rax, Reg64(h.reg));
        const auto ty = t.value_type == ir::Type::Void ? source.Operations()[t.value.value].type
                                                       : t.value_type;
        Mask(rax, TypeBits(ty));
        test(rax, rax);
        jnz(blocks[t.first.value], T_NEAR);
        jmp(blocks[t.second.value], T_NEAR);
        return;
    }
    if (t.kind == TerminalKind::Return) {
        MaterializeFlags();
        if (t.HasValue()) {
            if (source.Operations()[t.value.value].type == ir::Type::V128)
                Reject(CompileError::UnsupportedType);
            const auto& h = allocation.homes[t.value.value];
            if (h.spilled)
                mov(rax, qword[rsp + h.stack_offset]);
            else
                mov(rax, Reg64(h.reg));
            Mask(rax,
                 TypeBits(t.value_type == ir::Type::Void ? source.Operations()[t.value.value].type
                                                         : t.value_type));
        } else
            xor_(eax, eax);
        if (Linked()) {
            PublishFlags();
            Restore();
            HostAddress(r11, options.exit_handler, HostSymbol::Exit);
            jmp(r11);
        } else {
            PublishFlags();
            Restore();
            ret();
        }
        return;
    }
    if (t.kind != TerminalKind::Dispatch) Reject(CompileError::UnsupportedControlFlow);
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

void Writer::WriteResult(OpContext& c) {
    if (c.saved || c.operation.type == ir::Type::Void) return;
    const auto& h = allocation.homes[c.node.value];
    if (h.terminal_only) return;
    if (c.vector_result) {
        if (h.spilled)
            movdqu(ptr[rsp + h.stack_offset], c.vector);
        else if (c.vector.getIdx() != h.reg)
            movdqa(Xmm(h.reg), c.vector);
        if (!h.spilled) vector_residents[h.reg] = c.node.value;
    } else {
        if (!c.scalar_normalized && c.width == 32 && !h.spilled)
            mov(Reg32(h.reg), c.destination.cvt32());
        else {
            if (!c.scalar_normalized) Mask(c.destination, std::min(64u, c.width));
            if (h.spilled)
                mov(qword[rsp + h.stack_offset], c.destination);
            else if (c.destination.getIdx() != h.reg)
                mov(Reg64(h.reg), c.destination);
        }
        if (!h.spilled) scalar_residents[h.reg] = c.node.value;
    }
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

void Writer::Run() {
    if (options.arm_fpcr != UINT32_MAX && !runtime::arm64::IsSupportedFPCR(options.arm_fpcr))
        Reject(CompileError::UnsupportedOperand);
    if (Pinned()) {
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
    const bool fp_guard = allocation.cold_fp_count && options.arm_fpcr != UINT32_MAX &&
                          options.arm_fpcr_offset != UINT32_MAX;
    const auto enter = [&](u32 b, u64 pc) {
        flags = {};
        Prologue();
        if (options.local_flags && (allocation.flags_input[b] || fp_guard)) ImportLinkedFlags();
        if (fp_guard) {
            Label ready;
            cmp(dword[r12 + options.arm_fpcr_offset], options.arm_fpcr);
            je(ready, T_NEAR);
            guest_pc = pc;
            Exit(runtime::ExitValue(runtime::NativeExit::FPEnvironment));
            L(ready);
        }
    };
    enter(source.EntryBlock().value, options.entry_pc);
    std::vector<std::pair<unsigned, unsigned>> extra;
    for (auto b : allocation.block_order) {
        block = BlockId{b};
        L(blocks[b]);
        static_pc_known = false;
        has_fault_location = false;
        terminated = false;
        const auto& body = source.Blocks()[b];
        flags = {options.local_flags && allocation.flags_input[b] != 0,
                 false,
                 options.local_flags && allocation.flags_input[b] != 0,
                 false,
                 true};
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            current = NodeId{i};
            const auto& op = source.Operations()[i];
            if (op.opcode == OpCode::FaultLocation) {
                guest_pc = source.Operands(current).front().Payload();
                has_fault_location = true;
                continue;
            }
            if (op.opcode == OpCode::EntryPoint) {
                if (!Linked()) Reject(CompileError::UnsupportedControlFlow);
                const auto entry_pc = source.Operands(current).front().Payload();
                if (entry_pc == options.entry_pc) continue;
                output.entries.push_back({entry_pc, 0});
                extra.push_back({unsigned(output.entries.size() - 1), b});
                continue;
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
            if (allocation.homes[i].terminal_only) continue;
            PrepareArmFPRegion(c);
            PrepareFlags(c);
            Dispatch(c);
            WriteResult(c);
            if (op.flag_outputs && op.opcode != OpCode::Add && op.opcode != OpCode::Sub &&
                op.opcode != OpCode::Adc && op.opcode != OpCode::Sbb && op.opcode != OpCode::Neg &&
                op.opcode != OpCode::And && op.opcode != OpCode::Or && op.opcode != OpCode::Xor &&
                op.opcode != OpCode::AndNot && op.opcode != OpCode::Not)
                Reject(CompileError::UnsupportedFlags);
        }
        if (!terminated) Terminal(body.terminal);
    }
    EmitColdPaths();
    for (auto [index, b] : extra) {
        align(16);
        output.entries[index].offset = getSize();
        enter(b, output.entries[index].pc);
        jmp(blocks[b], T_NEAR);
    }
    while (getSize() & 3) db(0x90);
    ready(PROTECT_RW);
}
}  // namespace swift::runtime::backend::x64::detail
