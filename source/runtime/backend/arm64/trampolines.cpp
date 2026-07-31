//
// Created by 甘尧 on 2024/4/10.
//

#include "runtime/backend/cache_clear.h"
#include "runtime/backend/context.h"
#include "runtime/frontend/x86/cpu.h"
#include "trampolines.h"
#include "defines.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    // vixl label locations are non-negative byte offsets from the buffer
    // start (see vixl Label::GetLocation / IsBound).
    return_host = reinterpret_cast<ReturnHost>(code_buffer->exec_data +
                                               label_fault_return_host.GetLocation());
    call_host = reinterpret_cast<CallHost>(code_buffer->exec_data +
                                           label_call_host.GetLocation());
    const char* exec_map = std::getenv("SVM_EXEC_MAP");
    if (exec_map && std::strcmp(exec_map, "0") != 0) {
        std::fprintf(stderr,
                     "[svm-exec-map] trampoline=%p..%p entry=%p return=%p call=%p size=%zu\n",
                     static_cast<void*>(code_buffer->exec_data),
                     static_cast<void*>(code_buffer->exec_data + buffer_size),
                     reinterpret_cast<void*>(runtime_entry),
                     reinterpret_cast<void*>(return_host),
                     reinterpret_cast<void*>(call_host),
                     buffer_size);
    }

    // GPR registers can use
    // Mask convention (shared with the linear-scan pass and JitContext):
    // bit set = register unavailable (reserved here, or live during allocation),
    // bit clear = free to allocate. Start with everything free, then mark the
    // registers reserved for the runtime/trampoline ABI.
    gpr_regs.Reset(0);

#ifdef __APPLE__
    // X18 is reserved by the platform ABI
    gpr_regs.Mark(18);
#endif

    gpr_regs.Mark(x31.GetCode());  // sp
    // x29 is unavailable to linear scan in every mode, but W55 may use it as
    // the static RDX mapping. Mark it after validating static descriptors so
    // that the descriptor-overlap assertion still rejects every real runtime
    // ABI collision and duplicate descriptor.
    // x30 is the link register. Guest values live across a CallLambda must
    // never be allocated here: the host call and the generated block's final
    // return both require the architectural LR value.
    gpr_regs.Mark(x30.GetCode());
    gpr_regs.Mark(state.GetCode());
    gpr_regs.Mark(cache.GetCode());
    if (True(config.global_opts & Optimizations::ReturnStackBuffer)) {
        gpr_regs.Mark(rsb_ptr.GetCode());
    }
    if (config.page_table || config.memory_base) {
        gpr_regs.Mark(pt.GetCode());
        // Reserved scratch for bias folding in generated code (see
        // defines.h mem_scratch): never allocated to guest values.
        gpr_regs.Mark(mem_scratch.GetCode());
    }
    if (config.has_local_operation) {
        gpr_regs.Mark(local.GetCode());
    }
    gpr_regs.Mark(flags.GetCode());
    if (!backend::ScratchXPoolEnabled()) {
        gpr_regs.Mark(ip.GetCode());
        gpr_regs.Mark(atomic_scratch.GetCode());
        gpr_regs.Mark(atomic_pair_scratch.GetCode());
    }
    // vixl's MacroAssembler synthesizes un-encodable immediates and
    // out-of-range memory offsets through UseScratchRegisterScope, whose
    // default pool is tmp_list_ = {x16, x17} (macro-assembler-aarch64.cc).
    // Those writes are invisible to this allocator, so a guest value the
    // linear scan parked in x16/x17 is destroyed with no diagnostic. The
    // OFF retains the historical global reservation. ON hands the explicit
    // x11-x17 availability set to each emission's VIXL scratch scope, so an
    // allocator assignment and an implicit VIXL lease cannot overlap.
    if (!backend::ScratchXPoolEnabled()) {
        gpr_regs.Mark(ip0.GetCode());
        gpr_regs.Mark(ip1.GetCode());
    }

    // FPR registers can use
    fpr_regs.Reset(0);
    fpr_regs.Mark(ipv0.GetCode());
    fpr_regs.Mark(ipv1.GetCode());
    fpr_regs.Mark(ipv2.GetCode());
    fpr_regs.Mark(ipv3.GetCode());

    GPRSMask static_gprs{0};
    FPRSMask static_fprs{0};
    for (auto& desc : config.buffers_static_alloc) {
        if (desc.is_float) {
            ASSERT_MSG(!fpr_regs.Get(desc.reg) && !static_fprs.Get(desc.reg),
                       "static uniform FPR v{} overlaps the runtime ABI or "
                       "another static descriptor",
                       desc.reg);
            static_fprs.Mark(desc.reg);
            fpr_regs.Mark(desc.reg);
        } else {
            ASSERT_MSG(!gpr_regs.Get(desc.reg) && !static_gprs.Get(desc.reg),
                       "static uniform GPR x{} overlaps the runtime ABI or "
                       "another static descriptor",
                       desc.reg);
            static_gprs.Mark(desc.reg);
            gpr_regs.Mark(desc.reg);
        }
    }
    gpr_regs.Mark(fp.GetCode());  // fp / optional static RDX (x29)

    if (const char* dump = std::getenv("SVM_VIXL_HOST_DUMP");
        dump && std::strcmp(dump, "0") != 0) {
        const bool has_pt = config.page_table || config.memory_base;
        const bool pin_ext_level3 =
                static_gprs.Get(6) && static_gprs.Get(7) &&
                static_gprs.Get(8) && static_gprs.Get(9);
        std::fprintf(stderr,
                     "[svm-reg-mask] memory_base=%d page_table=%d "
                     "x24_reserved=%d x10_reserved=%d dispatcher_loc=x%d "
                     "pin_ext=%d pin_ext_level2=%d x0_x5_reserved=%d "
                     "pin_ext_level3=%d x6_x9_reserved=%d "
                     "xpool_requested=%d xpool_effective=%d xpool_auto_level3=%d "
                     "x22_reserved=%d x23_reserved=%d "
                     "x29_reserved=%d allocatable_gprs=%u\n",
                     config.memory_base != nullptr,
                     config.page_table != nullptr,
                     gpr_regs.Get(24),
                     gpr_regs.Get(10),
                     has_pt ? (pin_ext_level3 ? atomic_pair_scratch.GetCode()
                                              : ip6.GetCode())
                            : loc.GetCode(),
                     static_gprs.Get(22) && static_gprs.Get(23) && static_gprs.Get(29),
                     static_gprs.Get(0) && static_gprs.Get(1) && static_gprs.Get(2) &&
                             static_gprs.Get(3) && static_gprs.Get(4) && static_gprs.Get(5),
                     gpr_regs.Get(0) && gpr_regs.Get(1) && gpr_regs.Get(2) &&
                             gpr_regs.Get(3) && gpr_regs.Get(4) && gpr_regs.Get(5),
                     pin_ext_level3,
                     gpr_regs.Get(6) && gpr_regs.Get(7) &&
                             gpr_regs.Get(8) && gpr_regs.Get(9),
                     backend::ScratchXPoolRequested(),
                     backend::ScratchXPoolEnabled(),
                     backend::ScratchXPoolAutoEnabled(),
                     gpr_regs.Get(22),
                     gpr_regs.Get(23),
                     gpr_regs.Get(29),
                     static_cast<unsigned>(gpr_regs.GetClearCount()));
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
    // When guest addresses are virtualized (page_table / memory_base), x24 is
    // the persistent pt register (holds the guest->host bias for the whole
    // guest execution), so the dispatcher's scratch "current location" value
    // must live elsewhere. Levels 0-2 retain ip6/x9 byte-for-byte; level 3 pins
    // guest R15 there, so use dispatcher-only x13 instead. Dynamic values are
    // dead whenever control has returned to this dispatcher. Identity mode
    // retains the historical x24 (loc).
    const bool has_pt = config.page_table || config.memory_base;
    const bool scalar_insert =
            True(config.arm64_features & Arm64Features::AFP);
    // FEAT_AFP.NEP retains every lane except the scalar destination lane.
    // AH deliberately remains clear: changing it process-wide also changes
    // packed FP edge cases that are outside this optimisation's scope.
    constexpr u32 kFpcrNep = 1u << 2;
    const bool level3_x9_pin = std::any_of(
            config.buffers_static_alloc.begin(),
            config.buffers_static_alloc.end(),
            [](const auto& desc) { return !desc.is_float && desc.reg == 9; });
    const XRegister loc_reg = has_pt
            ? XRegister(level3_x9_pin ? atomic_pair_scratch.GetCode() : ip6.GetCode())
            : XRegister(loc.GetCode());
    bool caller_saved_static_pins = false;
    for (const auto& desc : config.buffers_static_alloc) {
        caller_saved_static_pins |= !desc.is_float && desc.reg <= 5;
    }
    // The historical dispatcher used w0 for the halt-reason probe and cache
    // miss value. At pin level 2, x0 is guest RSI and must survive until the
    // static-uniform spill (or the next linked block), so use the dispatcher
    // scratch x11 instead. Keep OFF/level-1 byte-identical.
    const WRegister halt_reg = caller_saved_static_pins ? ipw : w0;
    const bool exec_prof = [] {
        const char* env = std::getenv("SVM_EXEC_PROF");
        return env && std::strcmp(env, "0") != 0;
    }();
    auto record = [&](u32 offset) {
        if (!exec_prof) return;
        __ Ldr(ip0, MemOperand(state, state_offset_exec_profile_ptr));
        __ Ldr(ip1, MemOperand(ip0, offset));
        __ Add(ip1, ip1, 1);
        __ Str(ip1, MemOperand(ip0, offset));
    };
    __ Bind(&label_runtime_entry);
    BuildSaveHostCallee(assembler);
    if (scalar_insert) {
        // FEAT_AFP.NEP makes scalar Advanced SIMD instructions update only
        // lane 0. Keep the caller's FPCR on our stack, and expose the original
        // value to every host/C++ call below.
        __ Mrs(ip, FPCR);
        __ Stp(ip, xzr, MemOperand(sp, -16, PreIndex));
        __ Orr(ip, ip, kFpcrNep);
        __ Msr(FPCR, ip);
    }

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
    // Restore statically-allocated guest registers unconditionally: JitRun
    // always enters with a non-null forward, and the block-to-block path below
    // never restores them, so skipping this on the fast path would run blocks
    // with stale host values in statically-mapped guest registers.
    BuildRestoreStaticUniform(assembler);
    __ Cbnz(forward, &go_guest);

    // align loc
    __ Bind(&code_dispatcher);
    record(exec_offset_dispatch_entries);
    __ Ldr(loc_reg, MemOperand(state, state_offset_current_loc));
    __ Lsr(loc_index, loc_reg, 2);

    // query l1 cache
    __ Ldr(l1_cache, MemOperand(state, state_offset_l1_code_cache));
    __ Eor(l1_index, loc_index, Operand(loc_index, LSR, L1_CODE_CACHE_BITS));
    __ And(l1_index, l1_index, L1_CODE_CACHE_HASH);
    __ Add(l1_start, l1_cache, Operand(l1_index, LSL, 4));

    Label query_step_1;
    Label query_step_2;
    Label query_step_3;

    // l1 cache looper
    __ Bind(&query_step_1);
    __ Ldr(l1_index, MemOperand(l1_start, 0x10, PostIndex));
    __ Cbz(l1_index, &query_step_2);
    __ Sub(l1_index, l1_index, loc_reg);
    __ Cbnz(l1_index, &query_step_1);
    __ Ldr(forward, MemOperand(l1_start, -0x8));
    __ Cbz(forward, &query_step_2);
    record(exec_offset_dispatch_l1_hit);
    __ B(&go_guest);

    // query l2 cache
    __ Bind(&query_step_2);
    __ Eor(l2_index, loc_index, Operand(loc_index, LSR, L2_CODE_CACHE_BITS));
    __ And(l2_index, l2_index, L2_CODE_CACHE_HASH);
    __ Add(l2_start, l2_cache, Operand(l2_index, LSL, 4));

    // l2 looper
    __ Bind(&query_step_3);
    __ Ldr(l2_index, MemOperand(l2_start, 0x10, PostIndex));
    __ Cbz(l2_index, &code_cache_miss);
    __ Sub(l2_index, l2_index, loc_reg);
    __ Cbnz(l2_index, &query_step_3);
    __ Ldr(forward, MemOperand(l2_start, -0x8));
    __ Cbz(forward, &code_cache_miss);
    record(exec_offset_dispatch_l2_hit);

    // write to l1 cache
    __ Ldr(l2_index, MemOperand(l1_start, -0x8));
    __ Add(l2_index, l2_index, 1);
    __ Cbz(l2_index, &go_guest);  // check if l1 cache is full
    __ Stp(loc_reg, forward, MemOperand(l1_start, -0x10));

    __ Bind(&go_guest);
    if (config.enable_asm_interp) {
        __ Tbz(forward, 63, &jump_guest);
        __ Bind(&go_interp);
        __ Ldp(arg, handle, MemOperand(forward, 16, PostIndex));
        if (scalar_insert) {
            __ Ldr(ip, MemOperand(sp));
            __ Msr(FPCR, ip);
        }
        __ Blr(handle);
        if (scalar_insert) {
            __ Ldr(ip, MemOperand(sp));
            __ Orr(ip, ip, kFpcrNep);
            __ Msr(FPCR, ip);
        }
        __ Bfc(forward, 63, 1);
        __ Bind(&jump_guest);
    }
    __ Blr(forward);

    // Fault recovery enters before the halt-reason load. In pin levels 2/3,
    // x0 is guest RSI and halt_reg is x11, so recovery must publish PageFatal
    // through State rather than overwriting either register in the saved
    // machine context.
    __ Bind(&label_fault_return_host);
    // load exception
    __ Ldr(halt_reg, MemOperand(state, state_offset_halt_reason));
    __ Cbz(halt_reg, &code_dispatcher);
    __ Bind(&label_return_host);
    // clear execption
    __ Str(wzr, MemOperand(state, state_offset_halt_reason));
    // write back rsb
    __ Str(rsb_ptr, MemOperand(state, state_offset_rsb_pointer));
    __ Str(flags, MemOperand(state, state_offset_host_flags));
    BuildSaveStaticUniform(assembler);
    if (caller_saved_static_pins) {
        // x0 is the C-ABI return register for JitRun. Delay publishing the
        // halt reason until guest RSI has been written back above.
        __ Mov(w0, halt_reg);
    }
    if (scalar_insert) {
        __ Ldp(ip, ip0, MemOperand(sp, 16, PostIndex));
        __ Msr(FPCR, ip);
    }
    BuildRestoreHostCallee(assembler);
    __ Ret();

    __ Bind(&code_cache_miss);
    record(exec_offset_dispatch_miss);
    __ Mov(halt_reg, 0x8);
    __ B(&label_return_host);

    __ Bind(&label_call_host);
    __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
    __ Str(ipw, MemOperand(state, state_offset_halt_reason));
    __ Str(rsb_ptr, MemOperand(state, state_offset_rsb_pointer));
    __ Str(flags, MemOperand(state, state_offset_host_flags));
    BuildSaveStaticUniform(assembler);
    if (scalar_insert) {
        __ Ldr(ip, MemOperand(sp));
        __ Msr(FPCR, ip);
    }
    __ Mov(ip, reinterpret_cast<uintptr_t>(&TrampolinesArm64::CallHostTrampoline));
    __ Stp(x29, x30, MemOperand(sp, -16, PreIndex));
    __ Mov(x0, reinterpret_cast<uintptr_t>(this));
    __ Mov(x1, state);
    __ Blr(ip);
    __ Ldp(x29, x30, MemOperand(sp, 16, PostIndex));
    if (scalar_insert) {
        __ Ldr(ip, MemOperand(sp));
        __ Orr(ip, ip, kFpcrNep);
        __ Msr(FPCR, ip);
    }
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
