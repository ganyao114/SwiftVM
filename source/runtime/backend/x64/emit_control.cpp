#include "abi_layout.inc"
#include "emit.h"

namespace swift::runtime::backend::x64::detail {
void Writer::Handle(Op<OpCode::GuestSafepoint>, OpContext& c) {
    if (!Linked() || options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    Label ready;
    const auto flags_before = flags;
    if (options.local_flags) {
        cold_events.push_back({{}, flags_before, c.Arg(0).Payload()});
        if (!(flags.dirty && (flags.native || flags.floating))) {
            cmp(qword[r12 + SVM_X64_CTX_PENDING_EVENTS - SVM_X64_CTX_STATE], 0);
            jne(cold_events.back().entry, T_NEAR);
            return;
        }
        mov(rcx, qword[r12 + SVM_X64_CTX_PENDING_EVENTS - SVM_X64_CTX_STATE]);
        jrcxz(ready);
        jmp(cold_events.back().entry, T_NEAR);
        L(ready);
        return;
    }
    cmp(qword[r12 + SVM_X64_CTX_PENDING_EVENTS - SVM_X64_CTX_STATE], 0);
    je(ready, T_NEAR);
    const auto previous_pc = guest_pc;
    guest_pc = c.Arg(0).Payload();
    mov(rax, guest_pc);
    mov(qword[r12 + options.pc_offset], rax);
    Exit(runtime::ExitValue(runtime::NativeExit::GuestEvents));
    guest_pc = previous_pc;
    L(ready);
}

void Writer::Handle(Op<OpCode::GuestDispatchSafepoint>, OpContext& ctx) {
    if (InterruptibleDispatch()) return;
    Handle(Op<OpCode::GuestSafepoint>{}, ctx);
}

void Writer::SelectValue(OpContext& c) {
    if (options.local_flags && flags.dirty && c.operation.opcode == OpCode::CondSelect) {
        const auto condition = NativeCondition(static_cast<ir::Condition>(c.Arg(0).Payload()));
        if (c.vector_result) {
            Label other, done;
            if (condition != 16) NativeJump(condition ^ 1, other);
            ReadVector(c.Arg(1), xmm0);
            jmp(done, T_NEAR);
            L(other);
            ReadVector(c.Arg(2), xmm0);
            L(done);
        } else {
            Read(c.Arg(1), rax, true);
            Read(c.Arg(2), rcx, true);
            if (condition != 16) {
                db(0x48);
                db(0x0f);
                db(0x40 + (condition ^ 1));
                db(0xc1);
            }
            if (c.width < 32) {
                SaveNativeFlags();
                flags.native = false;
            }
        }
        return;
    }
    if (c.operation.opcode == OpCode::CondSelect) {
        Condition(rax, static_cast<ir::Condition>(c.Arg(0).Payload()));
        SaveNativeFlags();
        flags.native = false;
    } else
        Read(c.Arg(0), rax);
    test(rax, rax);
    Label other, done;
    if (c.operation.opcode == OpCode::SelectZero)
        jnz(other, T_NEAR);
    else
        jz(other, T_NEAR);
    if (c.vector_result)
        ReadVector(c.Arg(1), xmm0);
    else
        Read(c.Arg(1), rax, true);
    jmp(done, T_NEAR);
    L(other);
    if (c.vector_result)
        ReadVector(c.Arg(2), xmm0);
    else
        Read(c.Arg(2), rax, true);
    L(done);
}

#define SEL(name)                                                                                  \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { SelectValue(c); }
// clang-format off
SEL(Select)
SEL(SelectZero)
SEL(CondSelect)
// clang-format on
#undef SEL

void Writer::Handle(Op<OpCode::Goto>, OpContext& c) {
    if (options.local_flags) {
        SaveNativeFlags(true);
        flags.native = false;
    }
    Read(c.Arg(0), rax);
    WriteResult(c);
    c.saved = true;
    test(rax, rax);
    jnz(locals[c.node.value], T_NEAR);
}

void Writer::Handle(Op<OpCode::NotGoto>, OpContext& c) {
    if (options.local_flags) {
        SaveNativeFlags(true);
        flags.native = false;
    }
    Read(c.Arg(0), rax);
    WriteResult(c);
    c.saved = true;
    test(rax, rax);
    jz(locals[c.node.value], T_NEAR);
}

void Writer::Handle(Op<OpCode::BindLabel>, OpContext& c) {
    if (options.local_flags) SaveNativeFlags(true);
    L(locals[c.Arg(0).Payload()]);
    condition_saved = false;
    if (options.local_flags) flags = {true, false, true, false, true};
    static_pc_known = false;
}

void Writer::Handle(Op<OpCode::AdvancePC>, OpContext& c) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, c.Arg(0).Payload());
    add(qword[r12 + options.pc_offset], rax);
    static_pc_known = false;
}

void Writer::Handle(Op<OpCode::GetLocation>, OpContext&) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, qword[r12 + options.pc_offset]);
}

void Writer::Handle(Op<OpCode::SetLocation>, OpContext& c) {
    if (options.pc_offset == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, c.Arg(0).Payload());
    mov(qword[r12 + options.pc_offset], rax);
    static_pc = c.Arg(0).Payload();
    static_pc_known = true;
    guest_pc = static_pc;
    has_fault_location = true;
}

#define EMPTY(name)                                                                                \
    void Writer::Handle(Op<OpCode::name>, OpContext&) {}
// clang-format off
EMPTY(Nop)
EMPTY(FaultLocation)
EMPTY(EntryPoint)
EMPTY(UniformBarrier)
EMPTY(XchgBarrier)
EMPTY(BranchOnlyEdges)
// clang-format on
#undef EMPTY
#define NO(name)                                                                                   \
    void Writer::Handle(Op<OpCode::name>, OpContext&) { Reject(CompileError::UnsupportedOpcode); }
// clang-format off
NO(DefineLocal)
NO(LoadLocal)
NO(StoreLocal)
NO(AddPhi)
NO(LoadMemoryExtend)
NO(LoadMemoryExtendTSO)
NO(MemoryCopy)
NO(MemoryCopyTSO)
NO(GetHostGPR)
NO(GetHostFPR)
NO(SetHostGPR)
NO(SetHostFPR)
NO(PushRSB)
NO(CallReturn)
NO(PopRSB)
NO(CallLocation)
NO(CallDynamic)
// clang-format on
#undef NO
}  // namespace swift::runtime::backend::x64::detail
