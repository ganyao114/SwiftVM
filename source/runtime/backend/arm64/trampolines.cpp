//
// Created by 甘尧 on 2024/4/10.
//

#include "runtime/backend/cache_clear.h"
#include "runtime/backend/context.h"
#include "runtime/frontend/x86/cpu.h"
#include "trampolines.h"
#include "defines.h"

#define __ assembler.
namespace swift::runtime::backend::arm64 {

TrampolinesArm64::TrampolinesArm64(const Config& config) : Trampolines(config) { Build(); }

void TrampolinesArm64::Build() {
    MacroAssembler assembler{};
    BuildRuntimeEntry(assembler);
    __ FinalizeCode();
    auto buffer_size = __ GetBuffer() -> GetSizeInBytes();
    auto code_buffer = code_cache.AllocCode(buffer_size);
    std::memcpy(code_buffer->rw_data, __ GetBuffer()->GetStartAddress<u8*>(), buffer_size);
    code_buffer->Flush();
    runtime_entry = reinterpret_cast<RuntimeEntry>(code_buffer->exec_data);
    return_host = reinterpret_cast<ReturnHost>(
            code_buffer->exec_data +
            (__ GetBuffer()->GetStartAddress<ptrdiff_t>() - label_return_host.GetLocation()));
    call_host = reinterpret_cast<CallHost>(
            code_buffer->exec_data +
            (__ GetBuffer()->GetStartAddress<ptrdiff_t>() - label_return_host.GetLocation()));

    // GPR registers can use
    gpr_regs.Reset(UINT32_MAX);

#ifdef __APPLE__
    // X18 is tmp registers
    gpr_regs.Clear(18);
#endif

    gpr_regs.Clear(x31.GetCode());  // sp
    gpr_regs.Clear(fp.GetCode());   // fp
    gpr_regs.Clear(state.GetCode());
    gpr_regs.Clear(cache.GetCode());
    if (True(config.global_opts & Optimizations::ReturnStackBuffer)) {
        gpr_regs.Clear(rsb_ptr.GetCode());
    }
    if (config.page_table || config.memory_base) {
        gpr_regs.Clear(pt.GetCode());
    }
    if (config.has_local_operation) {
        gpr_regs.Clear(local.GetCode());
    }
    gpr_regs.Clear(flags.GetCode());
    gpr_regs.Clear(ip.GetCode());

    // FPR registers can use
    fpr_regs.Reset(UINT32_MAX);
    fpr_regs.Clear(ipv0.GetCode());
    fpr_regs.Clear(ipv1.GetCode());
    fpr_regs.Clear(ipv2.GetCode());
    fpr_regs.Clear(ipv3.GetCode());

    for (auto& desc : config.buffers_static_alloc) {
        if (desc.is_float) {
            fpr_regs.Clear(desc.reg);
        } else {
            gpr_regs.Clear(desc.reg);
        }
    }
}

#define loc_index ip0
#define l1_cache ip1
#define l1_index ip2
#define l1_start ip3
#define l2_cache cache
#define l2_index ip4
#define l2_start ip5
#define forward ip7

void TrampolinesArm64::BuildRuntimeEntry(MacroAssembler& assembler) {
    Label go_guest;
    Label code_dispatcher;
    Label go_interp;
    Label code_cache_miss;
    Label jump_guest;
    __ Bind(&label_runtime_entry);
    BuildSaveHostCallee(assembler);

    __ Mov(state, x0);
    __ Mov(forward, x1);
    // load cache_ptr
    __ Ldr(cache, MemOperand(state, state_offset_l2_code_cache));
    // load pt
    if (config.page_table || config.memory_base) {
        __ Ldr(pt, MemOperand(state, state_offset_pt));
    }
    __ Ldr(flags, MemOperand(state, state_offset_host_flags));
    // load local
    if (config.has_local_operation) {
        __ Ldr(local, MemOperand(state, state_offset_local_buffer));
    }
    // load rsb
    if (True(config.global_opts & Optimizations::ReturnStackBuffer)) {
        __ Ldr(rsb_ptr, MemOperand(state, state_offset_rsb_pointer));
    }
    __ Cbnz(forward, &go_guest);

    BuildRestoreStaticUniform(assembler);
    // align loc
    __ Bind(&code_dispatcher);
    __ Ldr(loc, MemOperand(state, state_offset_current_loc));
    __ Lsr(loc_index, loc, 2);

    // query l1 cache
    __ Ldr(l1_cache, MemOperand(state, state_offset_l1_code_cache));
    __ Eor(l1_index, loc_index, Operand(loc_index, LSR, L1_CODE_CACHE_BITS));
    __ And(l1_index, l1_index, L1_CODE_CACHE_HASH);
    __ And(l1_start, l1_cache, Operand(l1_index, LSL, L1_CODE_CACHE_HASH));

    Label query_step_1;
    Label query_step_2;
    Label query_step_3;

    // l1 cache looper
    __ Bind(&query_step_1);
    __ Ldr(l1_index, MemOperand(l1_start, 0x10, PostIndex));
    __ Cbz(l1_index, &query_step_2);
    __ Sub(l1_index, l1_index, loc);
    __ Cbnz(l1_index, &query_step_1);
    __ Ldr(forward, MemOperand(l1_start, -0x8));
    __ Cbnz(forward, &go_guest);

    // query l2 cache
    __ Bind(&query_step_2);
    __ Eor(l2_index, loc_index, Operand(loc_index, LSR, L2_CODE_CACHE_BITS));
    __ And(l2_index, l2_index, L2_CODE_CACHE_HASH);
    __ Add(l2_start, l2_cache, Operand(l2_index, LSL, 4));

    // l2 looper
    __ Bind(&query_step_3);
    __ Ldr(l2_index, MemOperand(l2_start, 0x10, PostIndex));
    __ Cbz(l2_index, &code_cache_miss);
    __ Sub(l2_index, l2_index, loc);
    __ Cbnz(l2_index, &query_step_3);
    __ Ldr(forward, MemOperand(l2_start, -0x8));
    __ Cbz(forward, &code_cache_miss);

    // write to l1 cache
    __ Ldr(l2_index, MemOperand(l1_start, -0x8));
    __ Add(l2_index, l2_index, 1);
    __ Cbz(l2_index, &go_guest);  // check if l1 cache is full
    __ Stp(loc, forward, MemOperand(l1_start, -0x10));

    __ Bind(&go_guest);
    if (config.enable_asm_interp) {
        __ Tbz(forward, 63, &jump_guest);
        __ Bind(&go_interp);
        __ Ldp(arg, handle, MemOperand(forward, 16, PostIndex));
        __ Blr(handle);
        __ Bfc(forward, 63, 1);
        __ Bind(&jump_guest);
    }
    __ Blr(forward);

    // load exception
    __ Ldr(w0, MemOperand(state, state_offset_halt_reason));
    __ Cbz(w0, &code_dispatcher);
    __ Bind(&label_return_host);
    // clear execption
    __ Str(wzr, MemOperand(state, state_offset_halt_reason));
    // write back rsb
    __ Str(rsb_ptr, MemOperand(state, state_offset_rsb_pointer));
    __ Str(flags, MemOperand(state, state_offset_host_flags));
    BuildSaveStaticUniform(assembler);
    BuildRestoreHostCallee(assembler);
    __ Ret();

    __ Bind(&code_cache_miss);
    __ Mov(w0, 0x8);
    __ B(&label_return_host);

    __ Bind(&label_call_host);
    __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
    __ Str(ipw, MemOperand(state, state_offset_halt_reason));
    __ Str(rsb_ptr, MemOperand(state, state_offset_rsb_pointer));
    __ Str(flags, MemOperand(state, state_offset_host_flags));
    BuildSaveStaticUniform(assembler);
    __ Mov(ip, reinterpret_cast<uintptr_t>(&TrampolinesArm64::CallHostTrampoline));
    __ Stp(x29, x30, MemOperand(sp, -16, PreIndex));
    __ Mov(x0, reinterpret_cast<uintptr_t>(this));
    __ Mov(x1, state);
    __ Blr(ip);
    __ Ldp(x29, x30, MemOperand(sp, 16, PostIndex));
    __ Ldr(rsb_ptr, MemOperand(state, state_offset_rsb_pointer));
    __ Ldr(flags, MemOperand(state, state_offset_host_flags));
    BuildRestoreStaticUniform(assembler);
}

#undef loc_index
#undef l1_cache
#undef l1_index
#undef l1_start
#undef l2_cache
#undef l2_index
#undef l2_start
#undef forward

void TrampolinesArm64::BuildSaveHostCallee(MacroAssembler& assembler) {
    __ Stp(x19, x20, MemOperand(sp, -16, PreIndex));
    __ Stp(x21, x22, MemOperand(sp, -16, PreIndex));
    __ Stp(x23, x24, MemOperand(sp, -16, PreIndex));
    __ Stp(x25, x26, MemOperand(sp, -16, PreIndex));
    __ Stp(x27, x28, MemOperand(sp, -16, PreIndex));
    __ Stp(x29, x30, MemOperand(sp, -16, PreIndex));

    __ Stp(q8, q9, MemOperand(sp, -32, PreIndex));
    __ Stp(q10, q11, MemOperand(sp, -32, PreIndex));
    __ Stp(q12, q13, MemOperand(sp, -32, PreIndex));
    __ Stp(q14, q15, MemOperand(sp, -32, PreIndex));
}

void TrampolinesArm64::BuildRestoreHostCallee(MacroAssembler& assembler) {
    __ Ldp(q14, q15, MemOperand(sp, 32, PostIndex));
    __ Ldp(q12, q13, MemOperand(sp, 32, PostIndex));
    __ Ldp(q10, q11, MemOperand(sp, 32, PostIndex));
    __ Ldp(q8, q9, MemOperand(sp, 32, PostIndex));

    __ Ldp(x29, x30, MemOperand(sp, 16, PostIndex));
    __ Ldp(x27, x28, MemOperand(sp, 16, PostIndex));
    __ Ldp(x25, x26, MemOperand(sp, 16, PostIndex));
    __ Ldp(x23, x24, MemOperand(sp, 16, PostIndex));
    __ Ldp(x21, x22, MemOperand(sp, 16, PostIndex));
    __ Ldp(x19, x20, MemOperand(sp, 16, PostIndex));
}

void TrampolinesArm64::BuildSaveStaticUniform(MacroAssembler& assembler) {
    for (int i = 0; i < config.buffers_static_alloc.size();) {
        auto& cur = config.buffers_static_alloc[i];
        if (i + 1 < config.buffers_static_alloc.size()) {
            auto& next = config.buffers_static_alloc[i + 1];
            auto div = next.offset - cur.offset;
            if (div == cur.size && div == next.size && cur.is_float == next.is_float) {
                if (div == sizeof(u128)) {
                    __ Stp(VRegister::GetQRegFromCode(cur.reg),
                           VRegister::GetQRegFromCode(next.reg),
                           MemOperand(state, state_offset_uniform_buffer + cur.offset));
                } else if (div == sizeof(u64)) {
                    if (cur.is_float) {
                        __ Stp(VRegister::GetDRegFromCode(cur.reg),
                               VRegister::GetDRegFromCode(next.reg),
                               MemOperand(state, state_offset_uniform_buffer + cur.offset));
                    } else {
                        __ Stp(XRegister(cur.reg),
                               XRegister(next.reg),
                               MemOperand(state, state_offset_uniform_buffer + cur.offset));
                    }
                } else if (div == sizeof(u32)) {
                    __ Stp(WRegister(cur.reg),
                           WRegister(next.reg),
                           MemOperand(state, state_offset_uniform_buffer + cur.offset));
                } else {
                    PANIC();
                }
                i += 2;
                continue;
            }
        }
        if (cur.size == sizeof(u128)) {
            __ Str(VRegister::GetQRegFromCode(cur.reg),
                   MemOperand(state, state_offset_uniform_buffer + cur.offset));
        } else if (cur.size == sizeof(u64)) {
            if (cur.is_float) {
                __ Str(VRegister::GetDRegFromCode(cur.reg),
                       MemOperand(state, state_offset_uniform_buffer + cur.offset));
            } else {
                __ Str(XRegister(cur.reg),
                       MemOperand(state, state_offset_uniform_buffer + cur.offset));
            }
        } else if (cur.size == sizeof(u32)) {
            __ Str(WRegister(cur.reg), MemOperand(state, state_offset_uniform_buffer + cur.offset));
        } else {
            PANIC();
        }
        i++;
    }
}

void TrampolinesArm64::BuildRestoreStaticUniform(MacroAssembler& assembler) {
    for (int i = 0; i < config.buffers_static_alloc.size();) {
        auto& cur = config.buffers_static_alloc[i];
        if (i + 1 < config.buffers_static_alloc.size()) {
            auto& next = config.buffers_static_alloc[i + 1];
            auto div = next.offset - cur.offset;
            if (div == cur.size && div == next.size && cur.is_float == next.is_float) {
                if (div == sizeof(u128)) {
                    __ Ldp(VRegister::GetQRegFromCode(cur.reg),
                           VRegister::GetQRegFromCode(next.reg),
                           MemOperand(state, state_offset_uniform_buffer + cur.offset));
                } else if (div == sizeof(u64)) {
                    if (cur.is_float) {
                        __ Ldp(VRegister::GetDRegFromCode(cur.reg),
                               VRegister::GetDRegFromCode(next.reg),
                               MemOperand(state, state_offset_uniform_buffer + cur.offset));
                    } else {
                        __ Ldp(XRegister(cur.reg),
                               XRegister(next.reg),
                               MemOperand(state, state_offset_uniform_buffer + cur.offset));
                    }
                } else if (div == sizeof(u32)) {
                    __ Ldp(WRegister(cur.reg),
                           WRegister(next.reg),
                           MemOperand(state, state_offset_uniform_buffer + cur.offset));
                } else {
                    PANIC();
                }
                i += 2;
                continue;
            }
        }
        if (cur.size == sizeof(u128)) {
            __ Ldr(VRegister::GetQRegFromCode(cur.reg),
                   MemOperand(state, state_offset_uniform_buffer + cur.offset));
        } else if (cur.size == sizeof(u64)) {
            if (cur.is_float) {
                __ Ldr(VRegister::GetDRegFromCode(cur.reg),
                       MemOperand(state, state_offset_uniform_buffer + cur.offset));
            } else {
                __ Ldr(XRegister(cur.reg),
                       MemOperand(state, state_offset_uniform_buffer + cur.offset));
            }
        } else if (cur.size == sizeof(u32)) {
            __ Ldr(WRegister(cur.reg), MemOperand(state, state_offset_uniform_buffer + cur.offset));
        } else {
            PANIC();
        }
        i++;
    }
}

bool TrampolinesArm64::LinkBlock(u8* source, u8* target, u8* source_rw, bool pic) {
    constexpr auto _4K = 1 << 12;
    constexpr auto _128MB = 1ULL << 27;
    constexpr auto _4G = 1ULL << 32;
    s64 offset = target - source;
    MacroAssembler masm{};
    if (std::abs(offset) >= _4G) {
        if (pic) {
            return false;
        }
        masm.Mov(ip, reinterpret_cast<VAddr>(target));
        masm.Br(ip);
    } else if (std::abs(offset) >= _128MB) {
        auto page_offset = reinterpret_cast<VAddr>(target) % _4K;
        Label label{};
        masm.Adrp(ip, &label);
        masm.Add(ip, ip, page_offset);
        masm.Br(ip);
        masm.BindToOffset(&label, offset);
    } else {
        Label label{};
        masm.B(&label);
        masm.BindToOffset(&label, offset);
    }
    masm.FinalizeCode();
    memcpy(source_rw,
           masm.GetBuffer()->GetStartAddress<void*>(),
           masm.GetBuffer()->GetSizeInBytes());
    ClearDCache(source_rw, 4 * 5);
    ClearDCache(target, 4 * 5);
    ClearICache(target, 4 * 5);
    return true;
}

void TrampolinesArm64::CallHostTrampoline(TrampolinesArm64* thiz, State* ctx) {
    auto pc = ctx->current_loc.Value();
    HostFunction* function{};
    {
        std::shared_lock guard(thiz->lock);
        if (auto itr = thiz->host_functions.find(pc); itr != thiz->host_functions.end()) {
            function = itr->second;
        } else {
            PANIC("Unbind function {}", pc);
        }
    }
}

std::optional<Trampolines::CallHost> TrampolinesArm64::GetCallHost(HostFunction* func,
                                                                   ISA frontend) {
    ASSERT(func);
    if (auto itr = call_host_trampolines.find(func->addr); itr != call_host_trampolines.end()) {
        return itr->second;
    }

    auto sig_hash = func->SignatureHash();
    if (auto itr = signature_trampolines.find(sig_hash); itr == signature_trampolines.end()) {
        MacroAssembler assembler{};
        auto trampoline = BuildFunctionTrampoline(assembler, func, frontend);
    }
}

void* TrampolinesArm64::BuildFunctionTrampoline(MacroAssembler& assembler,
                                                HostFunction* func,
                                                ISA frontend_isa) {
    auto empty_type = func->signatures.empty();
    auto ret_type = empty_type ? ParamType::Void : func->signatures[0];
    auto abi_desc = x86::GetABIDescriptor64();

    if (empty_type) {

    }
}

}  // namespace swift::runtime::backend::arm64
#undef __
