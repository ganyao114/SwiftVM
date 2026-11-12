#include "emit.h"

namespace swift::runtime::backend::x64::detail {
namespace {
void PinLoad(Xbyak::CodeGenerator& gen, const Xbyak::Reg64& target, unsigned reg, unsigned bits) {
    switch (bits) {
        case 64: gen.mov(target, Xbyak::Reg64(reg)); break;
        case 32: gen.mov(Xbyak::Reg32(target.getIdx()), Xbyak::Reg32(reg)); break;
        case 16: gen.movzx(Xbyak::Reg32(target.getIdx()), Xbyak::Reg16(reg)); break;
        case 8: gen.movzx(Xbyak::Reg32(target.getIdx()), Xbyak::Reg8(reg)); break;
        default: break;
    }
}
void PinStore(Xbyak::CodeGenerator& gen, unsigned reg, const Xbyak::Reg64& source, unsigned bits) {
    switch (bits) {
        case 64: gen.mov(Xbyak::Reg64(reg), source); break;
        case 32: gen.mov(Xbyak::Reg32(reg), Xbyak::Reg32(source.getIdx())); break;
        case 16: gen.mov(Xbyak::Reg16(reg), Xbyak::Reg16(source.getIdx())); break;
        case 8: gen.mov(Xbyak::Reg8(reg), Xbyak::Reg8(source.getIdx())); break;
        default: break;
    }
}
}  // namespace

void Writer::Uniform(OpContext& c) {
    auto at = c.operands.begin();
    const auto u = *at++;
    const auto off = u.Payload();
    if (off > INT32_MAX) Reject(CompileError::UnsupportedOperand);
    const auto width = c.operation.opcode == OpCode::LoadUniform ? c.width
                                                                : TypeBits(u.TypeView());
    const auto pinned = PinnedSlot(off, width / 8);
    const auto relative = pinned >= 0
                                  ? off - options.pinned_uniform_offset - pinned * 8
                                  : -1;
    if (c.operation.opcode == OpCode::LoadUniform) {
        if (c.vector_result)
            movdqu(xmm0, ptr[r12 + off]);
        else if (off == options.flags_offset)
            mov(rax, rbx);
        else if (pinned >= 0 && relative == 0 && (width == 64 || width == 32 || width == 16 ||
                                                   width == 8))
            PinLoad(*this, rax, PinnedRegister(static_cast<unsigned>(pinned)), width);
        else if (pinned >= 0) {
            // Rare in-slot access (for example AH): publish this slot, read
            // memory, then reload the register copy.
            mov(qword[r12 + options.pinned_uniform_offset + pinned * 8],
                Reg64(PinnedRegister(static_cast<unsigned>(pinned))));
            Load(rax, r12, off, c.width);
            mov(Reg64(PinnedRegister(static_cast<unsigned>(pinned))),
                qword[r12 + options.pinned_uniform_offset + pinned * 8]);
        } else
            Load(rax, r12, off, c.width);
        return;
    }
    const auto value = *at;
    if (View(value) == ir::Type::V128) {
        ReadVector(value, xmm0);
        movdqu(ptr[r12 + off], xmm0);
    } else {
        Read(value, rax);
        if (pinned >= 0 && relative == 0 &&
            (width == 64 || width == 32 || width == 16 || width == 8))
            PinStore(*this, PinnedRegister(static_cast<unsigned>(pinned)), rax, width);
        else if (pinned >= 0) {
            mov(qword[r12 + options.pinned_uniform_offset + pinned * 8],
                Reg64(PinnedRegister(static_cast<unsigned>(pinned))));
            Store(rax, r12, off, TypeBits(u.TypeView()));
            mov(Reg64(PinnedRegister(static_cast<unsigned>(pinned))),
                qword[r12 + options.pinned_uniform_offset + pinned * 8]);
        } else
            Store(rax, r12, off, TypeBits(u.TypeView()));
        if (off == options.flags_offset) mov(rbx, rax);
    }
    if (off == options.pc_offset) {
        static_pc_known = false;
        if (value.Kind() == OperandKind::Value) {
            const auto& op = source.Operations()[value.Payload()];
            if (op.opcode == OpCode::LoadImm) {
                static_pc = source.Operands(NodeId{u32(value.Payload())}).front().Payload();
                static_pc_known = true;
            }
        }
        // Unknown/EarlyOff frontends publish PC explicitly instead of
        // emitting FaultLocation. Those stores still identify the memory
        // accesses whose native faults the execution owner must recover.
        if (static_pc_known) {
            guest_pc = static_pc;
            has_fault_location = true;
        }
    }
}

void Writer::Handle(Op<OpCode::LoadUniform>, OpContext& c) { Uniform(c); }

void Writer::Handle(Op<OpCode::StoreUniform>, OpContext& c) { Uniform(c); }

void Writer::Handle(Op<OpCode::GetUniformAddress>, OpContext& c) {
    lea(rax, ptr[r12 + c.Arg(0).Payload()]);
}

void Writer::Memory(OpContext& c) {
    const bool load =
            c.operation.opcode == OpCode::LoadMemory || c.operation.opcode == OpCode::LoadMemoryTSO;
    auto at = c.operands.begin();
    Expression(at, c.operands.end(), rdi);
    if (load) {
        const auto first = static_cast<u32>(getSize());
        if (c.vector_result)
            movdqu(xmm0, ptr[rdi]);
        else
            Load(rax, rdi, 0, c.width);
        RecordGuestAccess(first, 1, c.vector_result ? 16 : c.width / 8);
    } else {
        const auto v = *at;
        if (View(v) == ir::Type::V128) {
            ReadVector(v, xmm0);
            const auto first = static_cast<u32>(getSize());
            movdqu(ptr[rdi], xmm0);
            RecordGuestAccess(first, 2, 16);
        } else {
            Read(v, rax);
            const auto first = static_cast<u32>(getSize());
            Store(rax, rdi, 0, TypeBits(View(v)));
            RecordGuestAccess(first, 2, TypeBits(View(v)) / 8);
        }
    }
}

#define MEM(name)                                                                                  \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Memory(c); }
// clang-format off
MEM(LoadMemory)
MEM(LoadMemoryTSO)
MEM(StoreMemory)
MEM(StoreMemoryTSO)
// clang-format on
#undef MEM

void Writer::Handle(Op<OpCode::MemoryBarrierTSO>, OpContext&) { mfence(); }

void Writer::Handle(Op<OpCode::MemoryStoreFence>, OpContext&) { sfence(); }

void Writer::Handle(Op<OpCode::MemoryLoadFence>, OpContext&) { lfence(); }

void Writer::Handle(Op<OpCode::CpuYield>, OpContext&) { pause(); }

void Writer::Handle(Op<OpCode::CheckMemoryAlignment>, OpContext& c) {
    Read(c.Arg(0), rax);
    mov(rcx, c.Arg(1).Payload());
    test(rax, rcx);
    Label ok;
    jz(ok, T_NEAR);
    Exit(ExitValue(NativeExit::AlignmentFault));
    L(ok);
}

void Writer::Handle(Op<OpCode::StoreUniformSelectImm>, OpContext& c) {
    const auto& body = source.Blocks()[block.value];
    if (Linked() && c.Arg(0).Payload() == options.pc_offset &&
        c.node.value + 1 == body.first_node + body.node_count &&
        body.terminal.kind == TerminalKind::Dispatch) {
        Read(c.Arg(1), rax);
        test(rax, rax);
        Label no;
        jz(no, T_NEAR);
        StaticExit(c.Arg(2).Payload());
        L(no);
        StaticExit(c.Arg(3).Payload());
        terminated = true;
        return;
    }
    Read(c.Arg(1), rax);
    test(rax, rax);
    mov(rax, c.Arg(3).Payload());
    mov(rdx, c.Arg(2).Payload());
    cmovnz(rax, rdx);
    const auto select_off = c.Arg(0).Payload();
    const auto select_pin = PinnedSlot(select_off, 8);
    if (select_pin >= 0)
        mov(qword[r12 + options.pinned_uniform_offset + select_pin * 8],
            Reg64(PinnedRegister(static_cast<unsigned>(select_pin))));
    Store(rax, r12, select_off, 64);
    if (select_pin >= 0)
        mov(Reg64(PinnedRegister(static_cast<unsigned>(select_pin))),
            qword[r12 + options.pinned_uniform_offset + select_pin * 8]);
    if (c.Arg(0).Payload() == options.pc_offset) static_pc_known = false;
}
}  // namespace swift::runtime::backend::x64::detail
