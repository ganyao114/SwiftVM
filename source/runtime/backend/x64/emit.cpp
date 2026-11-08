#include <algorithm>
#include <cstring>
#include "emit.h"

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
    frame_bytes = a.spill_bytes + (Linked() ? 0 : 24);
    mxcsr_slot = a.spill_bytes;
    output.frame_bytes = frame_bytes + (Linked() ? 0 : 48);
    output.spill_slots = a.spill_bytes / 8;
}

void Writer::Reject(CompileError e) { throw Rejected{e, current, block}; }

void Writer::PublishFlags() {
    if (options.flags_offset != UINT32_MAX) mov(qword[r12 + options.flags_offset], rbx);
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
    PublishFlags();
    if (reason && options.pc_offset != UINT32_MAX) {
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
}

void Writer::StaticExit(u64 pc) {
    PublishFlags();
    mov(rdi, pc);
    if (options.pc_offset != UINT32_MAX) mov(qword[r12 + options.pc_offset], rdi);
    Restore();
    if (!Linked()) {
        xor_(eax, eax);
        ret();
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
}

void Writer::Terminal(const StoredTerminal& t) {
    if (t.kind == TerminalKind::Jump) {
        jmp(blocks[t.first.value], T_NEAR);
        return;
    }
    if (t.kind == TerminalKind::Branch) {
        const auto& h = allocation.homes[t.value.value];
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
    PublishFlags();
    if (options.pc_offset != UINT32_MAX)
        mov(rdi, qword[r12 + options.pc_offset]);
    else
        xor_(edi, edi);
    Restore();
    if (Linked()) {
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
    if (c.vector_result) {
        if (h.spilled)
            movdqu(ptr[rsp + h.stack_offset], c.vector);
        else if (c.vector.getIdx() != h.reg)
            movdqa(Xmm(h.reg), c.vector);
        if (!h.spilled) vector_residents[h.reg] = c.node.value;
    } else {
        Mask(c.destination, std::min(64u, c.width));
        if (h.spilled)
            mov(qword[rsp + h.stack_offset], c.destination);
        else if (c.destination.getIdx() != h.reg)
            mov(Reg64(h.reg), c.destination);
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
    Prologue();
    std::vector<std::pair<unsigned, unsigned>> extra;
    for (auto b : allocation.block_order) {
        block = BlockId{b};
        L(blocks[b]);
        static_pc_known = false;
        terminated = false;
        const auto& body = source.Blocks()[b];
        for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
            current = NodeId{i};
            const auto& op = source.Operations()[i];
            if (op.opcode == OpCode::FaultLocation) {
                guest_pc = source.Operands(current).front().Payload();
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
            const auto first = getSize();
            fault_committed_flags = false;
            Dispatch(c);
            WriteResult(c);
            if (op.flag_outputs && op.opcode != OpCode::Add && op.opcode != OpCode::Sub &&
                op.opcode != OpCode::Adc && op.opcode != OpCode::Sbb && op.opcode != OpCode::Neg &&
                op.opcode != OpCode::And && op.opcode != OpCode::Or && op.opcode != OpCode::Xor &&
                op.opcode != OpCode::AndNot && op.opcode != OpCode::Not)
                Reject(CompileError::UnsupportedFlags);
            if (options.fault_locations && getSize() > first &&
                HasAnyEffect(EffectsFor(op.opcode, op.Metadata()), Effect::MayFault)) {
                output.faults.push_back({u32(first), u32(getSize()), guest_pc});
                output.faults.back().recovery[0] = fault_committed_flags;
            }
        }
        if (!terminated) Terminal(body.terminal);
    }
    for (auto [index, b] : extra) {
        align(16);
        output.entries[index].offset = getSize();
        Prologue();
        jmp(blocks[b], T_NEAR);
    }
    while (getSize() & 3) db(0x90);
    ready(PROTECT_RW);
}
}  // namespace swift::runtime::backend::x64::detail
