#include "abi_layout.inc"
#include "emit.h"
#include "helpers.h"
#include "relocate.h"
#include "runtime/arch/x86/sse42_string.h"
#include "runtime/arch/x86/x87.h"
#include "runtime/exit.h"

namespace swift::runtime::backend::x64::detail {
void Writer::PrepareCall() {
    if (Linked()) mov(qword[r12 + SVM_X64_CTX_PHASE - SVM_X64_CTX_STATE], SVM_NATIVE_PHASE_HELPER);
    saved_scalar.clear();
    saved_vectors.clear();
    const auto pos = allocation.homes[current.value].definition_position;
    for (unsigned reg : {8u, 9u}) {
        auto id = scalar_residents[reg];
        if (id != NodeId::invalid && allocation.homes[id].last_use >= pos) {
            mov(qword[rsp + allocation.call_offset + 192 + (reg - 8) * 8], Reg64(reg));
            saved_scalar.push_back(reg);
        }
    }
    for (unsigned reg = 2; reg < 14; ++reg) {
        auto id = vector_residents[reg];
        if (id != NodeId::invalid && allocation.homes[id].last_use >= pos) {
            movdqu(ptr[rsp + allocation.call_offset + (reg - 2) * 16], Xmm(reg));
            saved_vectors.push_back(reg);
        }
    }
    PublishFlags();
    if (options.native_fp_environment && Linked()) {
        stmxcsr(dword[r12 - 40]);
        ldmxcsr(dword[r12 - 56]);
    } else if (!Linked()) {
        if (options.native_fp_environment && options.mxcsr_offset != UINT32_MAX)
            stmxcsr(dword[rsp + mxcsr_slot + 4]);
        ldmxcsr(dword[rsp + mxcsr_slot]);
    }
    in_call = true;
}

void Writer::FinishCall() {
    if (options.native_fp_environment && options.mxcsr_offset != UINT32_MAX) {
        if (Linked())
            ldmxcsr(dword[r12 - 40]);
        else
            ldmxcsr(dword[rsp + mxcsr_slot + 4]);
    }
    for (auto reg : saved_vectors)
        movdqu(Xmm(reg), ptr[rsp + allocation.call_offset + (reg - 2) * 16]);
    for (auto reg : saved_scalar)
        mov(Reg64(reg), qword[rsp + allocation.call_offset + 192 + (reg - 8) * 8]);
    if (options.flags_offset != UINT32_MAX) mov(rbx, qword[r12 + options.flags_offset]);
    if (Linked()) mov(qword[r12 + SVM_X64_CTX_PHASE - SVM_X64_CTX_STATE], SVM_NATIVE_PHASE_GUEST);
    in_call = false;
}

void Writer::HostAddress(const Reg64& reg, std::uintptr_t address, HostSymbol symbol) {
    db(0x48 | ((reg.getIdx() >> 3) & 1));
    db(0xb8 + (reg.getIdx() & 7));
    const auto offset = getSize();
    dq(address);
    if (!options.relocatable) return;
    if (symbol == HostSymbol::None) symbol = CallbackSymbol(address);
    if (symbol == HostSymbol::None) output.relocatable = false;
    if (symbol != HostSymbol::None)
        output.relocations.push_back({u32(offset), RelocationKind::X64Immediate64, symbol, 0});
}

void Writer::Call(std::uintptr_t target, HostSymbol symbol) {
    HostAddress(r11, target, symbol);
    call(r11);
    FinishCall();
}

void Writer::CallRegister(const Reg64& reg) {
    call(reg);
    FinishCall();
}

void Writer::Handle(Op<OpCode::CallLambda>, OpContext& c) {
    if (c.count < 2 || c.count > 5 || c.Arg(0).Kind() != OperandKind::Lambda)
        Reject(CompileError::UnsupportedOperand);
    PrepareCall();
    const auto target = c.Arg(1);
    if (target.Kind() == OperandKind::Immediate)
        HostAddress(r11, target.Payload(), HostSymbol::None);
    else {
        output.relocatable = false;
        Read(target, r11, true);
    }
    constexpr std::array<unsigned, 3> abi{7, 6, 2};
    for (unsigned i = 2; i < c.count; ++i) Read(c.Arg(i), Reg64(abi[i - 2]));
    CallRegister(r11);
    if (c.vector_result) {
        movq(xmm0, rax);
        movq(xmm1, rdx);
        punpcklqdq(xmm0, xmm1);
    }
}

void Writer::Query(OpContext& c) {
    const bool cpu = c.operation.opcode == OpCode::Cpuid,
               pair = c.operation.opcode == OpCode::ReadSystemPair;
    auto target = c.Arg(cpu ? 2 : 1).Payload();
    PrepareCall();
    if (cpu) {
        Read(c.Arg(0), rdi);
        Read(c.Arg(1), rsi);
    } else {
        mov(rdi, c.Arg(0).Payload());
        xor_(esi, esi);
    }
    Call(target, HostSymbol::None);
    if (cpu) mov(qword[rsp + allocation.homes[c.node.value].auxiliary_offset + 8], rdx);
    if (pair) {
        movq(xmm0, rax);
        movq(xmm1, rdx);
        punpcklqdq(xmm0, xmm1);
    }
}

#define Q(name)                                                                                    \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Query(c); }
// clang-format off
Q(Cpuid)
Q(ReadSystem)
Q(ReadSystemPair)
// clang-format on
#undef Q

void Writer::Handle(Op<OpCode::CpuidUpper>, OpContext& c) {
    auto off = allocation.homes[c.Arg(0).Payload()].auxiliary_offset;
    if (off == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, qword[rsp + off + 8]);
}

void Writer::Handle(Op<OpCode::Div128>, OpContext& c) {
    const unsigned bits = TypeBits(View(c.Arg(0)));
    PrepareCall();
    Read(c.Arg(0), rdi);
    Read(c.Arg(1), rsi);
    Read(c.Arg(2), rdx);
    mov(ecx, bits | (c.Arg(3).Payload() << 8));
    const auto off = allocation.homes[c.node.value].auxiliary_offset;
    lea(r8, ptr[rsp + off]);
    Call(reinterpret_cast<std::uintptr_t>(&WideDivide), HostSymbol::X64WideDivide);
    mov(qword[rsp + off + 8], rdx);
    Label ok;
    cmp(qword[rsp + off], 0);
    jne(ok, T_NEAR);
    Exit(ExitValue(NativeExit::DivideFault));
    L(ok);
}

void Writer::Handle(Op<OpCode::Div128Remainder>, OpContext& c) {
    const auto off = allocation.homes[c.Arg(0).Payload()].auxiliary_offset;
    if (off == UINT32_MAX) Reject(CompileError::UnsupportedOperand);
    mov(rax, qword[rsp + off + 8]);
}

void Writer::X87(OpContext& c) {
    const bool checked = c.operation.opcode == OpCode::X87Execute;
    PrepareCall();
    Read(c.Arg(0), rdi, true);
    mov(rsi, c.Arg(1).Payload());
    Read(c.Arg(2), rdx, true);
    if (checked) {
        mov(rcx, guest_pc);
        Read(c.Arg(3), r8, true);
        mov(r9, c.Arg(4).Payload());
        Call(reinterpret_cast<std::uintptr_t>(&CheckedX87), HostSymbol::X64CheckedX87);
    } else
        Call(reinterpret_cast<std::uintptr_t>(&SvmX87Execute), HostSymbol::X87Execute);
    Label ok;
    test(rax, rax);
    jz(ok, T_NEAR);
    Exit(ExitValue(NativeExit::X87Fault));
    L(ok);
    xor_(eax, eax);
}

void Writer::Handle(Op<OpCode::X87Op>, OpContext& c) { X87(c); }

void Writer::Handle(Op<OpCode::X87Execute>, OpContext& c) { X87(c); }

void Writer::Handle(Op<OpCode::X87CommitMetadata>, OpContext& c) {
    using namespace runtime::x86;
    Read(c.Arg(0), rdi, true);
    mov(rax, c.Arg(1).Payload());
    Store(rax, rdi, X87MetadataOffset(c.Arg(3).Payload(), 0), 64);
    if (X87MetadataHasDP(c.Arg(3).Payload())) {
        Read(c.Arg(2), rax, true);
        Store(rax, rdi, X87MetadataOffset(c.Arg(3).Payload(), 1), 64);
    }
    mov(rax, X87MetadataOpcode(c.Arg(3).Payload()));
    Store(rax, rdi, X87MetadataOffset(c.Arg(3).Payload(), 2), 16);
}

void Writer::String(OpContext& c) {
    const bool ex = c.operation.opcode == OpCode::Sse42StrExplicit;
    PrepareCall();
    ReadVector(c.Arg(0), xmm0);
    ReadVector(c.Arg(1), xmm1);
    movq(rdi, xmm0);
    movhlps(xmm14, xmm0);
    movq(rsi, xmm14);
    movq(rdx, xmm1);
    movhlps(xmm14, xmm1);
    movq(rcx, xmm14);
    if (ex) {
        ReadVector(c.Arg(2), xmm0);
        movq(r8, xmm0);
        movhlps(xmm14, xmm0);
        movq(r9, xmm14);
        sub(rsp, 16);
        mov(qword[rsp], c.Arg(3).Payload());
        HostAddress(r11,
                    reinterpret_cast<std::uintptr_t>(&runtime::x86::sse42::CompareExplicit),
                    HostSymbol::StringExplicit);
        call(r11);
        add(rsp, 16);
        FinishCall();
    } else {
        mov(r8, c.Arg(2).Payload());
        Call(reinterpret_cast<std::uintptr_t>(&runtime::x86::sse42::CompareImplicit),
             HostSymbol::StringImplicit);
    }
}

void Writer::Handle(Op<OpCode::Sse42Str>, OpContext& c) { String(c); }

void Writer::Handle(Op<OpCode::Sse42StrExplicit>, OpContext& c) { String(c); }
}  // namespace swift::runtime::backend::x64::detail
