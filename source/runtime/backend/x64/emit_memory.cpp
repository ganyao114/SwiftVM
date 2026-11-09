#include "emit.h"

namespace swift::runtime::backend::x64::detail {
void Writer::Uniform(OpContext& c) {
    auto at = c.operands.begin();
    const auto u = *at++;
    const auto off = u.Payload();
    if (off > INT32_MAX) Reject(CompileError::UnsupportedOperand);
    if (c.operation.opcode == OpCode::LoadUniform) {
        if (c.vector_result)
            movdqu(xmm0, ptr[r12 + off]);
        else if (off == options.flags_offset)
            mov(rax, rbx);
        else
            Load(rax, r12, off, c.width);
        return;
    }
    const auto value = *at;
    if (View(value) == ir::Type::V128) {
        ReadVector(value, xmm0);
        movdqu(ptr[r12 + off], xmm0);
    } else {
        Read(value, rax);
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
    Store(rax, r12, c.Arg(0).Payload(), 64);
    if (c.Arg(0).Payload() == options.pc_offset) static_pc_known = false;
}
}  // namespace swift::runtime::backend::x64::detail
