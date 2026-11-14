#include "emit.h"

#include "runtime/arch/arm64/numeric.h"

namespace swift::runtime::backend::x64::detail {
void Writer::EmitFPConstants() {
    constexpr u64 values[]{UINT64_C(0x7fffffff7fffffff),
                           UINT64_C(0x0080000000800000),
                           UINT64_C(0x7fffffffffffffff),
                           UINT64_C(0x0010000000000000)};
    for (unsigned index = 0; index < fp_constants.size(); ++index) {
        if (!fp_constants_used[index]) continue;
        align(16);
        L(fp_constants[index]);
        dq(values[index]);
        dq(values[index]);
    }
}

void Writer::EmitFPColdPaths(u32 tmp) {
    for (auto& path : cold_fp) {
        current = path.node;
        L(path.entry);
        xmm0_node = NodeId::invalid;
        flags = path.flags;
        if (flags.dirty) SaveNativeFlags(true);
        auto input = source.Operands(path.node).begin();
        ReadVector(*input++, xmm0);
        ReadVector(*input, xmm1);
        if (options.avx) {
            vmovdqu(ptr[rsp + tmp], xmm0);
            vmovdqu(ptr[rsp + tmp + 16], xmm1);
        } else {
            movdqu(ptr[rsp + tmp], xmm0);
            movdqu(ptr[rsp + tmp + 16], xmm1);
        }
        call(fp_stubs[path.stub].entry);
        if (path.dest_reg != 0) {
            if (options.avx)
                vmovdqa(Xmm(path.dest_reg), xmm0);
            else
                movdqa(Xmm(path.dest_reg), xmm0);
        }
        if (path.flags.dirty) {
            flags = {true, false, true, false, true};
            if (path.flags.saved && !path.flags.saved_carry) {
                if (options.lahf)
                    xor_(word[rsp + allocation.flags_slot], 0x100);
                else
                    xor_(qword[rsp + allocation.flags_slot], 1);
                flags.saved_carry = false;
            }
            if (path.flags.native) {
                EnsureNativeFlags();
                if (flags.carry != path.flags.carry) cmc();
            }
        }
        if (path.fp_const >= 0) vmovdqu(xmm15, ptr[rip + fp_constants[path.fp_const]]);
        if (path.bcast_node != NodeId::invalid)
            EmitIndexedFPBroadcast(
                    path.bcast_node, (path.bcast_key & 0x10) ? 64 : 32, path.bcast_key & 0xf);
        jmp(path.resume, T_NEAR);
    }
}

void Writer::EmitFPCallStubs(u32 tmp) {
    for (auto& stub : fp_stubs) {
        align(16);
        L(stub.entry);
        sub(rsp, 8);
        stack_bias = 16;
        flags = {stub.key.flags_saved, false, stub.key.flags_saved, false, true};
        arm_fp_region = stub.key.region;
        arm_fp_control = stub.key.control;
        arm_fp_status = stub.key.status;
        PrepareCall({stub.key.scalars, stub.key.vectors});
        lea(rdi, ptr[rsp + tmp + stack_bias + 32]);
        lea(rsi, ptr[rsp + tmp + stack_bias]);
        lea(rdx, ptr[rsp + tmp + stack_bias + 16]);
        mov(ecx, stub.key.format);
        if (IsImmediateArmFPCR(stub.key.control))
            mov(r8d, stub.key.control & ~kArmImmediateFPCR);
        else if (options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change ||
                 stub.key.control != options.arm_fpcr_offset)
            mov(r8d, dword[r12 + stub.key.control]);
        else
            mov(r8d, options.arm_fpcr);
        lea(r9, ptr[r12 + stub.key.status]);
        Call(reinterpret_cast<std::uintptr_t>(&runtime::arm64::EvaluateVectorBinaryFP),
             HostSymbol::ArmFPBinary);
        if (options.avx)
            vmovdqu(xmm0, ptr[rsp + tmp + stack_bias + 32]);
        else
            movdqu(xmm0, ptr[rsp + tmp + stack_bias + 32]);
        if (stub.key.flags_saved) ImportPackedFlags();
        if (stub.key.region) BeginArmFPRegion(stub.key.control, stub.key.status);
        add(rsp, 8);
        ret();
        stack_bias = 0;
    }
}

void Writer::EmitNumericColdPaths(u32 tmp) {
    for (auto& path : cold_numeric) {
        current = path.node;
        L(path.entry);
        xmm0_node = NodeId::invalid;
        OpContext context{source.Operations()[current.value], current, source.Operands(current)};
        for (const auto input : context.operands) context.args[context.count++] = input;
        context.width = TypeBits(context.operation.type);
        if (context.operation.opcode == OpCode::CallLambda) {
            flags = {};
            PrepareCall(path.masks);
            LambdaCall(context);
            jmp(path.resume, T_NEAR);
            continue;
        }
        flags = path.flags;
        if (flags.dirty && (flags.native || flags.floating || flags.saved != 15))
            SaveNativeFlags();
        flags.native = 0;
        const auto encoded = context.Arg(context.count - 1).Payload();
        const auto info = runtime::arm64::DescribeFPValues(encoded);
        const auto offsets = context.Arg(context.count - 2).Payload();
        const unsigned inputs = context.count - 2;
        for (unsigned i = 0; i < inputs; ++i) {
            if (info.kind == runtime::arm64::FPValueKind::FromInt && !info.vector_int) {
                Read(context.Arg(i), rax);
                if (options.avx)
                    vmovq(xmm0, rax);
                else
                    movq(xmm0, rax);
            } else {
                ReadVector(context.Arg(i), xmm0);
            }
            if (options.avx)
                vmovdqu(ptr[rsp + tmp + i * 16], xmm0);
            else
                movdqu(ptr[rsp + tmp + i * 16], xmm0);
        }
        const NumericCallKey key{runtime::arm64::CanonicalFPValues(encoded, info),
                                 u32(offsets),
                                 u32(offsets >> 32),
                                 path.masks.scalars,
                                 path.masks.vectors,
                                 flags.dirty,
                                 flags.saved_carry};
        const auto [stub, inserted] = numeric_stub_index.try_emplace(key, numeric_stubs.size());
        if (inserted) numeric_stubs.push_back({key, {}});
        call(numeric_stubs[stub->second].entry);
        flags = {};
        if (!IsImmediateArmFPCR(key.control) &&
            (options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change ||
             key.control != options.arm_fpcr_offset)) {
            Label ready;
            test(eax, eax);
            jz(ready, T_NEAR);
            guest_pc = path.pc;
            Exit(runtime::ExitValue(runtime::NativeExit::FPEnvironment));
            L(ready);
        }
        if (info.kind == runtime::arm64::FPValueKind::Compare) {
            mov(rdx, qword[rsp + tmp + 48]);
            if (options.local_flags) {
                mov(eax, 0x202);
                mov(ecx, 0x203);
                cmp(edx, 8);
                cmove(eax, ecx);
                mov(ecx, 0x242);
                cmp(edx, 6);
                cmove(eax, ecx);
                mov(ecx, 0x247);
                cmp(edx, 3);
                cmove(eax, ecx);
                if (options.lahf) {
                    shl(eax, 8);
                    sahf();
                } else {
                    push(rax);
                    popfq();
                }
            } else {
                shl(rdx, 28);
                MergeFlags(rdx, 15);
            }
        } else if (info.kind == runtime::arm64::FPValueKind::ToInt && !info.vector_int) {
            mov(rax, qword[rsp + tmp + 48]);
        } else if (options.avx) {
            vmovdqu(Xmm(path.dest_reg), ptr[rsp + tmp + 48]);
        } else {
            movdqu(Xmm(path.dest_reg), ptr[rsp + tmp + 48]);
        }
        if (path.fp_const >= 0) vmovdqu(xmm15, ptr[rip + fp_constants[path.fp_const]]);
        if (path.bcast_node != NodeId::invalid)
            EmitIndexedFPBroadcast(
                    path.bcast_node, (path.bcast_key & 0x10) ? 64 : 32, path.bcast_key & 0xf);
        jmp(path.resume, T_NEAR);
    }
}

void Writer::EmitNumericCallStubs(u32 tmp) {
    for (auto& stub : numeric_stubs) {
        align(16);
        L(stub.entry);
        sub(rsp, 8);
        stack_bias = 16;
        flags = {stub.key.flags_saved, false, stub.key.flags_saved, false, stub.key.saved_carry};
        PrepareCall({stub.key.scalars, stub.key.vectors});
        lea(rdi, ptr[rsp + tmp + stack_bias + 48]);
        lea(rsi, ptr[rsp + tmp + stack_bias]);
        mov(rdx, stub.key.encoded);
        if (IsImmediateArmFPCR(stub.key.control))
            mov(ecx, stub.key.control & ~kArmImmediateFPCR);
        else if (options.arm_fpcr == UINT32_MAX || allocation.fpcr_may_change ||
                 stub.key.control != options.arm_fpcr_offset)
            mov(ecx, dword[r12 + stub.key.control]);
        else
            mov(ecx, options.arm_fpcr);
        lea(r8, ptr[r12 + stub.key.status]);
        Call(reinterpret_cast<std::uintptr_t>(&runtime::arm64::EvaluateFPValues),
             HostSymbol::ArmFPValues);
        add(rsp, 8);
        ret();
        stack_bias = 0;
    }
}

void Writer::EmitEventColdPaths() {
    for (auto& path : cold_events) {
        L(path.entry);
        flags = path.flags;
        guest_pc = path.pc;
        mov(rax, path.pc);
        mov(qword[r12 + options.pc_offset], rax);
        Exit(runtime::ExitValue(runtime::NativeExit::GuestEvents));
    }
}

void Writer::EmitColdPaths() {
    const auto tmp = allocation.call_offset + 224;
    EmitFPColdPaths(tmp);
    EmitFPCallStubs(tmp);
    arm_fp_region = false;
    EmitNumericColdPaths(tmp);
    EmitNumericCallStubs(tmp);
    EmitEventColdPaths();
}
}  // namespace swift::runtime::backend::x64::detail
