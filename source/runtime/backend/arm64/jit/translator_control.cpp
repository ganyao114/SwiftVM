#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/common/helper_abi.h"
#include "runtime/frontend/x86/x87.h"
#include "translator/x86/cpu.h"

namespace swift::x86 {
u64 XsaveHelper(u64 context, u64 guest_address, u64 rfbm);
u64 XsavecHelper(u64 context, u64 guest_address, u64 rfbm);
u64 XrstorHelper(u64 context, u64 guest_address, u64 rfbm);
}  // namespace swift::x86

namespace swift::runtime::backend::arm64 {

namespace {

bool LeafHelperABIEnabled() {
    static const bool enabled = [] {
#if SVM_HAS_HELPER_PRESERVE_ALL
        const char* value = PerfGetenv("SVM_HELPER_LEAF_ABI");
        return value && std::strcmp(value, "0") != 0;
#else
        return false;
#endif
    }();
    return enabled;
}

}  // namespace

#define __ masm.

void JitTranslator::SpillStaticFPRUniforms() {
    for (const auto& desc : context.GetConfig().buffers_static_alloc) {
        if (!desc.is_float) continue;
        ASSERT_MSG(desc.size == sizeof(u128),
                   "direct-context helper sync only supports V128 static uniforms");
        __ Str(VRegister::GetQRegFromCode(desc.reg),
               MemOperand(state, state_offset_uniform_buffer + desc.offset));
    }
}

void JitTranslator::RestoreStaticFPRUniforms() {
    for (const auto& desc : context.GetConfig().buffers_static_alloc) {
        if (!desc.is_float) continue;
        ASSERT_MSG(desc.size == sizeof(u128),
                   "direct-context helper sync only supports V128 static uniforms");
        __ Ldr(VRegister::GetQRegFromCode(desc.reg),
               MemOperand(state, state_offset_uniform_buffer + desc.offset));
    }
}

void JitTranslator::EmitAdvancePC(ir::Inst* inst) {
    if (backedge_flags_plan && backedge_flags_plan->optimized &&
        backedge_flags_plan->final_advance == inst) {
        // This is the last architectural boundary before the proven self/
        // single-cold terminal. Keep the requested NZCV live for the self
        // edge; both cold exits materialize it in their veneers.
        flags_set = ir::Flags::None;
        flags_clear = ir::Flags::None;
        return;
    }
    MergeNZCV();
    FlushFlags();
}

void JitTranslator::EmitPopRSB(ir::Inst* inst) {
    // Deliberate no-op: the frontend emits PopRSB immediately before Return(),
    // so the block always ends in a PopRSBHint terminal. The actual RSB pop +
    // predict is emitted there (see EmitTerminal) after guest flags have been
    // flushed — emitting the direct-branch pop here, mid-block with flags still
    // in host NZCV, would hand the return target stale flags. This instruction
    // is kept as a frontend marker pairing the ret with its RSB frame.
}

void JitTranslator::EmitNotGoto(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    if (auto local = LocalConditionFor(cond)) {
        __ B(GetLocalLabel(inst),
             static_cast<Condition>(static_cast<u8>(*local) ^ 1));
        return;
    }
    __ Cbz(context.W(cond), GetLocalLabel(inst));
}

void JitTranslator::EmitGoto(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    if (auto local = LocalConditionFor(cond)) {
        __ B(GetLocalLabel(inst), *local);
        return;
    }
    __ Cbnz(context.W(cond), GetLocalLabel(inst));
}

void JitTranslator::EmitBindLabel(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    __ Bind(GetLocalLabel(value.Def()));
}

void JitTranslator::EmitPushRSB(ir::Inst* inst) {
    // When RSB is disabled rsb_ptr (x25) is neither reserved in the register
    // mask nor loaded at runtime entry, so emitting any push would clobber an
    // allocated guest register — bail out entirely.
    if (False(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
        return;
    }
    // The argument is the guest return address: Lambda(Imm{pc}) for a call.
    // A dynamic (value) target has no statically known return address here, so
    // there is nothing to predict — skip.
    auto lambda = inst->GetArg<ir::Lambda>(0);
    if (lambda.IsValue()) {
        return;
    }
    const u64 ret_addr = lambda.GetImm().Get();
    // Reserve the L2 dispatch slot for the return target now; the slot's value
    // word is filled with the code pointer once that target is compiled, so the
    // matching pop can branch directly to it (or fall back if still 0).
    context.EmitRSBPush(ret_addr, context.GetDispatchIndex(ret_addr));
}

void JitTranslator::EmitHostCall(const ir::Lambda& lambda,
                                 const std::vector<ir::DataClass>& args,
                                 bool has_result,
                                 const Register& result) {
    ASSERT(args.size() <= 8);
    MergeNZCV();
    FlushFlags();

    // Materialize value arguments before taking the register snapshot. In
    // function mode an argument can be RegAlloc::MEM; context.X() then reloads
    // it into a caller-saved scratch register. If that reload happens after
    // the snapshot and argument setup subsequently reads the register's saved
    // slot, it passes the stale pre-reload value to the helper.
    std::vector<XRegister> value_args;
    value_args.reserve(args.size());
    for (const auto& data : args) {
        if (data.IsValue()) {
            value_args.emplace_back(context.X(data.value));
        }
    }
    std::optional<XRegister> lambda_value;
    if (lambda.IsValue()) {
        lambda_value.emplace(context.X(lambda.GetValue()));
    }

    // Most helpers receive only ordinary scalar arguments and cannot observe
    // the uniform buffer. XSAVE/XRSTOR are the exception: they take a
    // ThreadContext64* and directly read/write xmms[]. Keep the boundary
    // narrow so x87/SoftFloat helpers do not pay sixteen state stores/loads.
    bool sync_xmm_before = false;
    bool sync_xmm_after = false;
    if (!lambda.IsValue()) {
        const auto target = lambda.GetImm().Get();
        sync_xmm_before =
                target == reinterpret_cast<VAddr>(&swift::x86::XsaveHelper) ||
                target == reinterpret_cast<VAddr>(&swift::x86::XsavecHelper) ||
                target == reinterpret_cast<VAddr>(&swift::x86::XrstorHelper);
        sync_xmm_after =
                target == reinterpret_cast<VAddr>(&swift::x86::XrstorHelper);
    }
    if (sync_xmm_before) {
        SpillStaticFPRUniforms();
    }

    // The metadata is present only on direct helpers whose definition carries
    // the same preserve_all convention. The process-constant switch defaults
    // off, leaving the established AAPCS snapshot byte-for-byte unchanged.
    // Unsupported compilers cannot enable the path even if the environment
    // variable is present.
    const bool preserve_all_leaf =
            LeafHelperABIEnabled() && !lambda.IsValue() &&
            lambda.GetHelperABI() == ir::HelperABI::PreserveAllLeaf;
    const bool fpcr_transparent =
            sse_afp_nan && !lambda.IsValue() &&
            lambda.GetHostFpEffect() == ir::HostFpEffect::FPCRTransparent;
    const bool unsafe_skip_fpcr =
            sse_afp_nan && fpcr_tax_skip_switch && !fpcr_transparent;

    // Save the caller-saved registers that are actually live across the call,
    // plus x29/x30: the Blr below clobbers the link register holding this
    // block's return address back to the dispatcher.  x18 is reserved on
    // Apple; x19+ are callee-saved and preserved by the helper itself.
    //
    // SIMD registers are saved as full 128-bit values, because the host ABI
    // only guarantees the low 64 bits of v8-v15 and the function-wide
    // allocator can keep a V128 live across CallLambda.
    //
    // "Live" is context.GetLiveGPRs/GetLiveFPRs: RegAlloc's per-instruction
    // live set (a conservative superset), the runtime's reserved registers,
    // and every scratch handed out while emitting this instruction -- which
    // includes the reloads context.X() just did above for spilled arguments.
    // A register outside those masks holds nothing this block will read again.
    //
    // This used to save all 17 caller-saved GPRs and all 32 Q registers
    // unconditionally: 672 bytes of stack and ~94 instructions at every call
    // site, which is 90.8% of the emitted code on the x87 default path
    // (docs/perf-baseline.md 5.3).  An integer-only block has just the four
    // reserved ipv0-ipv3 marked out of 32 FPRs.
    GPRSMask live_gprs = context.GetLiveGPRs();
    GPRSMask argument_gprs{};
    // Argument registers are unioned in explicitly instead of trusting the
    // mask to contain them.  The argument setup below reads each one back from
    // its save slot (loading x0 first would otherwise clobber a later
    // argument's source register), so a register with no slot would silently
    // pass garbage to the helper -- the worst failure shape available here.
    for (const auto& reg : value_args) {
        if (reg.GetCode() <= 17) {
            live_gprs.Mark(reg.GetCode());
            argument_gprs.Mark(reg.GetCode());
        }
    }
    if (lambda_value && lambda_value->GetCode() <= 17) {
        live_gprs.Mark(lambda_value->GetCode());
        argument_gprs.Mark(lambda_value->GetCode());
    }
    // SVM_X86_PIN_EXT=2/3 maps guest RSI-R15 to AAPCS64 caller-saved x0-x9.
    // They are architectural state even when no allocator value is live here:
    // argument setup and BLR may clobber every one of them. Union the static
    // descriptors explicitly so this correctness boundary does not depend on
    // the allocator mask containing reserved registers.
    for (const auto& desc : context.GetConfig().buffers_static_alloc) {
        // preserve_all keeps x9-x15. x9 therefore needs no snapshot unless it
        // is an argument source; x0-x8 remain caller-saved.
        if (!desc.is_float && desc.reg <= (preserve_all_leaf ? 8 : 9)) {
            live_gprs.Mark(desc.reg);
        }
    }

    boost::container::small_vector<u32, 18> save_gprs;
    for (u32 code = 0; code <= 17; ++code) {
        const bool leaf_clobbered = code <= 8 || code >= 16;
        if (live_gprs.Get(code) &&
            (!preserve_all_leaf || leaf_clobbered || argument_gprs.Get(code))) {
            save_gprs.push_back(code);
        }
    }
    boost::container::small_vector<u32, 32> save_fprs;
    const FPRSMask& live_fprs = context.GetLiveFPRs();
    for (u32 code = 0; code < 32; ++code) {
        if (live_fprs.Get(code) && (!preserve_all_leaf || code <= 7)) {
            save_fprs.push_back(code);
        }
    }

    if (RAShapeProfEnabled()) {
        const auto abi = lambda.IsValue()
                ? RAShapeHelperABI::IndirectAAPCS
                : (preserve_all_leaf
                           ? RAShapeHelperABI::DirectPreserveAll
                           : (sync_xmm_before || sync_xmm_after
                                      ? RAShapeHelperABI::XStateSyncAAPCS
                                      : RAShapeHelperABI::DirectAAPCS));
        RAShapeHelperCounters call{};
        call.calls = 1;
        // Save+restore of the live caller-saved GPR/FPR sets, plus the
        // explicit x29/x30 link pair. Argument loads and the result slot are
        // call plumbing, not caller-state snapshots, and stay out of this
        // counter by definition.
        call.snapshot_instructions =
                2 * ((save_gprs.size() + 1) / 2 +
                     (save_fprs.size() + 1) / 2 + 1);
        call.snapshot_code_bytes = call.snapshot_instructions * 4;
        call.snapshot_memory_bytes =
                2 * (save_gprs.size() * sizeof(u64) +
                     save_fprs.size() * sizeof(u128) + 2 * sizeof(u64));
        u64 static_fprs = 0;
        for (const auto& desc : context.GetConfig().buffers_static_alloc) {
            static_fprs += desc.is_float;
        }
        call.xmm_sync_instructions =
                static_fprs * (u64(sync_xmm_before) + u64(sync_xmm_after));
        call.xmm_sync_memory_bytes = call.xmm_sync_instructions * sizeof(u128);
        auto& total = context.GetRAShapeCounters().helpers[static_cast<size_t>(abi)];
        total.calls += call.calls;
        total.snapshot_instructions += call.snapshot_instructions;
        total.snapshot_code_bytes += call.snapshot_code_bytes;
        total.snapshot_memory_bytes += call.snapshot_memory_bytes;
        total.xmm_sync_instructions += call.xmm_sync_instructions;
        total.xmm_sync_memory_bytes += call.xmm_sync_memory_bytes;
        if (!lambda.IsValue()) {
            RAShapeRecordHelperTarget(lambda.GetImm().Get(), abi, call);
        }
    }

    std::array<int, 18> gpr_slot{};
    gpr_slot.fill(-1);
    u32 cursor{0};
    for (u32 code : save_gprs) {
        gpr_slot[code] = int(cursor);
        cursor += 8;
    }
    const u32 kLinkSlot = cursor;
    cursor += 16;
    const u32 kResultSlot = cursor;
    cursor += 8;
    // sp must stay 16-byte aligned, and the Q accesses below want a 16-byte
    // multiple as their base.
    const u32 kSimdSaveOffset = (cursor + 15u) & ~15u;
    const u32 kSnapshotSaveBytes =
            (kSimdSaveOffset + u32(save_fprs.size()) * 16u + 15u) & ~15u;
    // Diagnostic timing keeps only the current sample pointer in the helper
    // frame; timestamps are written directly to the Runtime-private buffer.
    const u32 kTimingSampleSlot = kSnapshotSaveBytes;
    const u32 kSaveBytes = kSnapshotSaveBytes + (fpcr_tax_timing ? 16u : 0u);
    auto saved_offset = [&](u32 code) -> u32 {
        ASSERT_MSG(code < gpr_slot.size() && gpr_slot[code] >= 0,
                   "host call argument in an unsaved register");
        return u32(gpr_slot[code]);
    };

    __ Sub(sp, sp, kSaveBytes);
    for (size_t i = 0; i + 1 < save_gprs.size(); i += 2) {
        __ Stp(XRegister(save_gprs[i]),
               XRegister(save_gprs[i + 1]),
               MemOperand(sp, gpr_slot[save_gprs[i]]));
    }
    if (save_gprs.size() & 1u) {
        __ Str(XRegister(save_gprs.back()), MemOperand(sp, gpr_slot[save_gprs.back()]));
    }
    __ Stp(x29, x30, MemOperand(sp, kLinkSlot));
    for (size_t i = 0; i + 1 < save_fprs.size(); i += 2) {
        __ Stp(VRegister::GetQRegFromCode(save_fprs[i]),
               VRegister::GetQRegFromCode(save_fprs[i + 1]),
               MemOperand(sp, kSimdSaveOffset + u32(i) * 16));
    }
    if (save_fprs.size() & 1u) {
        __ Str(VRegister::GetQRegFromCode(save_fprs.back()),
               MemOperand(sp, kSimdSaveOffset + u32(save_fprs.size() - 1) * 16));
    }

    // Load arguments into x0-x7.
    u32 index{0};
    u32 value_index{0};
    for (auto& data : args) {
        auto dst = XRegister(index++);
        if (data.IsImm()) {
            __ Mov(dst, data.imm.Get());
        } else {
            auto src = value_args[value_index++];
            if (src.GetCode() <= 17) {
                __ Ldr(dst, MemOperand(sp, saved_offset(src.GetCode())));
            } else {
                __ Mov(dst, src);
            }
        }
    }

    auto begin_fpcr_timing = [&] {
        if (!fpcr_tax_timing) return;
        Label no_sample;
        Label dropped;
        __ Str(xzr, MemOperand(sp, kTimingSampleSlot));
        __ Ldr(ip0, MemOperand(state, state_offset_exec_profile_ptr));
        __ Ldr(ip0, MemOperand(ip0, profile_offset_fpcr_timing));
        __ Cbz(ip0, &no_sample);
        __ Ldr(ip1, MemOperand(ip0, offsetof(FpcrTimingBuffer, calls)));
        __ Add(ip1, ip1, 1);
        __ Str(ip1, MemOperand(ip0, offsetof(FpcrTimingBuffer, calls)));
        if (lambda.IsValue()) {
            __ Tst(ip1, kFpcrTimingSampleMask);
        } else {
            // A plain global call-count modulus can phase-lock to a periodic
            // helper sequence and omit an entire target. Give each static
            // target a different sampling phase while retaining 1/1024.
            __ Mov(ip, lambda.GetImm().Get());
            __ Eor(ip, ip1, Operand(ip, LSR, 4));
            __ Tst(ip, kFpcrTimingSampleMask);
        }
        __ B(&no_sample, ne);
        __ Ldr(ip1, MemOperand(ip0, offsetof(FpcrTimingBuffer, sample_count)));
        __ Cmp(ip1, kFpcrTimingMaxSamples);
        __ B(&dropped, hs);
        __ Mov(ip, sizeof(FpcrTimingSample));
        __ Madd(ip2, ip1, ip, ip0);
        __ Add(ip2, ip2, offsetof(FpcrTimingBuffer, samples));
        __ Add(ip1, ip1, 1);
        __ Str(ip1, MemOperand(ip0, offsetof(FpcrTimingBuffer, sample_count)));
        __ Str(ip2, MemOperand(sp, kTimingSampleSlot));
        if (lambda.IsValue()) {
            __ Str(xzr, MemOperand(ip2, offsetof(FpcrTimingSample, target)));
        } else {
            __ Mov(ip1, lambda.GetImm().Get());
            __ Str(ip1, MemOperand(ip2, offsetof(FpcrTimingSample, target)));
        }
        if (args.size() > 1) {
            __ Str(x1, MemOperand(ip2, offsetof(FpcrTimingSample, arg1)));
        } else {
            __ Str(xzr, MemOperand(ip2, offsetof(FpcrTimingSample, arg1)));
        }
        __ Mov(ip1, fpcr_transparent ? 1 : 0);
        __ Str(ip1,
               MemOperand(ip2,
                          offsetof(FpcrTimingSample, fpcr_transparent)));
        __ Mrs(ip1, CNTVCT_EL0);
        __ Str(ip1, MemOperand(ip2, offsetof(FpcrTimingSample, start)));
        __ B(&no_sample);
        __ Bind(&dropped);
        __ Ldr(ip1, MemOperand(ip0, offsetof(FpcrTimingBuffer, dropped)));
        __ Add(ip1, ip1, 1);
        __ Str(ip1, MemOperand(ip0, offsetof(FpcrTimingBuffer, dropped)));
        __ Bind(&no_sample);
    };
    auto mark_fpcr_timing = [&](size_t offset) {
        if (!fpcr_tax_timing) return;
        Label no_sample;
        __ Ldr(ip0, MemOperand(sp, kTimingSampleSlot));
        __ Cbz(ip0, &no_sample);
        __ Mrs(ip1, CNTVCT_EL0);
        __ Str(ip1, MemOperand(ip0, offset));
        __ Bind(&no_sample);
    };

    // Conservative helpers execute under the caller's native FP environment.
    // A direct call site may explicitly certify FPCR transparency; only that
    // narrow allowlist keeps the AFP guest FPCR installed across BLR. The
    // runtime-entry frame sits immediately above this helper frame.
    if (sse_afp_nan) {
        context.RecordFpcrTaxCounter(FpcrTaxCounter::DirectHelper);
        if (fpcr_transparent) {
            context.RecordFpcrTaxCounter(FpcrTaxCounter::DirectHelperFPFree);
        }
    }
    begin_fpcr_timing();
    if (sse_afp_nan && !fpcr_transparent) {
        if (unsafe_skip_fpcr) {
            // UNSAFE diagnostic only. Preserve the production ON layout so a
            // wall-clock recovery cannot be misattributed to code movement.
            // These two NOPs replace LDR host_fpcr + MSR FPCR exactly.
            __ Nop();
            __ Nop();
        } else {
            __ Ldr(ip0,
                   MemOperand(sp, kSaveBytes + kSseAFPHostFPCROffset));
            __ Msr(FPCR, ip0);
        }
    }
    mark_fpcr_timing(offsetof(FpcrTimingSample, host_ready));

    // Function address.
    if (lambda.IsValue()) {
        auto fn = *lambda_value;
        if (fn.GetCode() <= 17) {
            __ Ldr(ip, MemOperand(sp, saved_offset(fn.GetCode())));
        } else {
            __ Mov(ip, fn);
        }
    } else {
        // Fixed-width materialization: vixl's Mov() elides zero 16-bit
        // chunks, which makes the emitted length depend on where ASLR placed
        // the helper in *this* process (a page-aligned helper such as RepMovs
        // drops a chunk with probability ~1/4 per process).  The function-mode
        // fingerprint's self-consistency check compares host byte counts
        // across two processes, so chunk elision reads as nondeterministic
        // codegen.  movz+3xmovk is always 16 bytes and stays scannable by
        // code_serial's relocation pass.
        const u64 target = lambda.GetImm().Get();
        __ movz(ip, target & 0xFFFFu, 0);
        __ movk(ip, (target >> 16) & 0xFFFFu, 16);
        __ movk(ip, (target >> 32) & 0xFFFFu, 32);
        __ movk(ip, (target >> 48) & 0xFFFFu, 48);
    }
    __ Blr(ip);

    mark_fpcr_timing(offsetof(FpcrTimingSample, helper_done));
    __ Str(x0, MemOperand(sp, kResultSlot));
    if (sse_afp_nan && !fpcr_transparent) {
        if (unsafe_skip_fpcr) {
            // Match the 16 static instructions in the cache restore emitter
            // (hit + inline miss body). SVM_FPCR_TAX_PROF must stay off for
            // this layout-controlled diagnostic because its counter snippets
            // intentionally add probe-only code to the production path.
            for (u32 i = 0; i < 16; ++i) __ Nop();
        } else {
            // A helper such as XRSTOR or a NaN cold handler may have updated
            // context.mxcsr. Every helper takes the same compare path; none is
            // trusted through a target-specific cleanliness exemption.
            EmitSseAFPRestoreGuestFPCRCached(
                    masm,
                    state,
                    kSaveBytes,
                    ip,
                    ip0,
                    ip1,
                    [this](FpcrTaxCounter counter) {
                        context.RecordFpcrTaxCounter(counter);
                    });
        }
    }
    mark_fpcr_timing(offsetof(FpcrTimingSample, guest_ready));

    for (size_t i = 0; i + 1 < save_fprs.size(); i += 2) {
        __ Ldp(VRegister::GetQRegFromCode(save_fprs[i]),
               VRegister::GetQRegFromCode(save_fprs[i + 1]),
               MemOperand(sp, kSimdSaveOffset + u32(i) * 16));
    }
    if (save_fprs.size() & 1u) {
        __ Ldr(VRegister::GetQRegFromCode(save_fprs.back()),
               MemOperand(sp, kSimdSaveOffset + u32(save_fprs.size() - 1) * 16));
    }
    for (size_t i = 0; i + 1 < save_gprs.size(); i += 2) {
        __ Ldp(XRegister(save_gprs[i]),
               XRegister(save_gprs[i + 1]),
               MemOperand(sp, gpr_slot[save_gprs[i]]));
    }
    if (save_gprs.size() & 1u) {
        __ Ldr(XRegister(save_gprs.back()), MemOperand(sp, gpr_slot[save_gprs.back()]));
    }
    __ Ldp(x29, x30, MemOperand(sp, kLinkSlot));
    if (has_result) {
        __ Ldr(result, MemOperand(sp, kResultSlot));
    }
    __ Add(sp, sp, kSaveBytes);
    if (sync_xmm_after) {
        RestoreStaticFPRUniforms();
    }
}

void JitTranslator::EmitCallLambda(ir::Inst* inst) {
    auto lambda = inst->GetArg<ir::Lambda>(0);
    std::vector<ir::DataClass> args{};
    for (int i = 1; i < 4; i++) {
        if (inst->ArgAt(i).IsValue()) {
            args.emplace_back(inst->GetArg<ir::Value>(i));
        } else if (inst->ArgAt(i).IsImm()) {
            args.emplace_back(inst->GetArg<ir::Imm>(i));
        }
    }
    auto self = ir::Value{inst};
    auto has_result = context.HasAllocation(self);
    Register result{};
    if (has_result) {
        result = context.R(self);
    }
    EmitHostCall(lambda, args, has_result, result);
}

void JitTranslator::EmitGetOperand(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto result = context.R(ir::Value{inst});
    if ((mem_narrow_fuse || addr_ea_tie) && operand.GetRight().Null() &&
        operand.GetLeft().IsValue() && inst->GetUses() == 1 &&
        context.SharesGPR(ir::Value{inst}, operand.GetLeft().value)) {
        bool feeds_memory = false;
        auto it = cur_block->GetInstList().iterator_to(*inst);
        for (++it; it != cur_block->GetInstList().end(); ++it) {
            bool uses_address = false;
            for (auto used : it->GetValues()) {
                uses_address |= used.Def() == inst;
            }
            if (!uses_address) {
                continue;
            }
            feeds_memory = it->GetOp() == ir::OpCode::LoadMemory ||
                           it->GetOp() == ir::OpCode::StoreMemory ||
                           it->GetOp() == ir::OpCode::LoadMemoryTSO ||
                           it->GetOp() == ir::OpCode::StoreMemoryTSO;
            break;
        }
        if (feeds_memory) {
            // EmitMemOperand consumes this result's tied register directly.
            return;
        }
    }
    if (!mem_narrow_fuse || operand.GetRight().Null()) {
        __ Mov(result, EmitOperand(operand));
        return;
    }

    // GetOperand is the materialised guest EA used by memory IR. EmitOperand
    // historically built a composite address in scratch and this final Mov
    // transported it into `result`. Computing into the allocated destination
    // directly is identical arithmetic and preserves the later memory access
    // (hence its fault point), while removing that transport instruction.
    Register left;
    if (operand.GetLeft().IsImm()) {
        __ Mov(result, operand.GetLeft().imm.Get());
        left = result;
    } else {
        left = context.R(operand.GetLeft().value, true);
    }
    Register dst = left.Is64Bits() ? result.X() : result.W();
    auto right = operand.GetRight();
    if (right.IsImm()) {
        const auto imm = right.imm.GetSigned();
        if (operand.GetOp() == ir::OperandOp::Plus) {
            if (__ IsImmAddSub(imm)) {
                __ Add(dst, left, imm);
            } else {
                __ Mov(dst, imm);
                __ Add(dst, left, dst);
            }
        } else if (operand.GetOp() == ir::OperandOp::LSL) {
            __ Lsl(dst, left, imm);
        } else if (operand.GetOp() == ir::OperandOp::LSR) {
            __ Lsr(dst, left, imm);
        } else {
            PANIC();
        }
        return;
    }

    auto right_reg = context.R(right.value, true);
    if (operand.GetOp() == ir::OperandOp::Plus) {
        __ Add(dst, left, right_reg);
    } else if (operand.GetOp() == ir::OperandOp::LSL) {
        __ Lsl(dst, left, right_reg);
    } else if (operand.GetOp() == ir::OperandOp::LSR) {
        __ Lsr(dst, left, right_reg);
    } else if (operand.GetOp() == ir::OperandOp::PlusExt) {
        __ Add(dst, left, Operand{right_reg, LSL, operand.GetOp().shift_ext});
    } else {
        PANIC();
    }
}

void JitTranslator::EmitCallDynamic(ir::Inst* inst) {
    auto lambda = inst->GetArg<ir::Lambda>(0);
    auto params = inst->GetArg<ir::Params>(1);
    std::vector<ir::DataClass> args{};
    for (auto& param : params) {
        args.emplace_back(param.data);
    }
    auto self = ir::Value{inst};
    auto has_result = context.HasAllocation(self);
    Register result{};
    if (has_result) {
        result = context.R(self);
    }
    EmitHostCall(lambda, args, has_result, result);
}

void JitTranslator::EmitDefineLocal(ir::Inst* inst) {}

void JitTranslator::EmitGetLocation(ir::Inst* inst) {
    __ Ldr(context.X(ir::Value{inst}), MemOperand(state, state_offset_current_loc));
}

void JitTranslator::EmitSetLocation(ir::Inst* inst) {
    auto location = inst->GetArg<ir::Lambda>(0);
    // Any SetLocation invalidates an earlier constant, including a dynamic one:
    // SetLocation is *not* only emitted just before a terminal (decoder_x87.cc
    // and decoder_xsave.cc plant the faulting PC mid-block), so a block can
    // hold a constant SetLocation followed by the `jmp *rax` one -- and reusing
    // the stale constant there would turn an indirect jump into a jump back
    // into the middle of the same block.
    static_next_loc.reset();
    if (location.IsValue()) {
        __ Str(context.X(location.GetValue()), MemOperand(state, state_offset_current_loc));
    } else {
        __ Mov(ip, location.GetImm().Get());
        __ Str(ip, MemOperand(state, state_offset_current_loc));
        // Remember the constant for the terminal: Translate(ir::Inst*) clears
        // this again for every instruction that is not a SetLocation, so it
        // only survives to the terminal when nothing could have changed
        // state->current_loc in between.
        static_next_loc = location.GetImm().Get();
    }
}

void JitTranslator::EmitCheckMemoryAlignment(ir::Inst* inst) {
    const auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto mask = inst->GetArg<ir::Imm>(1).Get();
    Label aligned;

    // Tst clobbers NZCV, so commit any pending guest flags first. On failure,
    // Ret returns to the runtime-entry dispatcher, which observes PageFatal in
    // State::halt_reason and exits through the normal guest-fault path.
    MergeNZCV();
    __ Tst(address, mask);
    __ B(&aligned, eq);
    __ Mov(ipw, static_cast<u32>(HaltReason::PageFatal));
    __ Str(ipw, MemOperand(state, state_offset_halt_reason));
    __ Ret();
    __ Bind(&aligned);
}

void JitTranslator::EmitCallLocation(ir::Inst* inst) {
    // TODO: semantics assumed to be a host C-ABI call with params, same as CallDynamic.
    auto lambda = inst->GetArg<ir::Lambda>(0);
    auto params = inst->GetArg<ir::Params>(1);
    std::vector<ir::DataClass> args{};
    for (auto& param : params) {
        args.emplace_back(param.data);
    }
    auto self = ir::Value{inst};
    auto has_result = context.HasAllocation(self);
    Register result{};
    if (has_result) {
        result = context.R(self);
    }
    EmitHostCall(lambda, args, has_result, result);
}

void JitTranslator::EmitNop(ir::Inst* inst) { __ Nop(); }

}  // namespace swift::runtime::backend::arm64
