#include "translator.h"

#include <array>
#include <limits>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/pair_call_trampoline.h"
#include "runtime/backend/context.h"
#include "runtime/common/svm_config.h"
#include "runtime/frontend/x86/sse42str_helper.h"
#include "runtime/frontend/x86/x87.h"
#include "translator/x86/cpu.h"

namespace swift::x86 {
u64 XsaveHelper(u64 context, u64 guest_address, u64 rfbm);
u64 XsavecHelper(u64 context, u64 guest_address, u64 rfbm);
u64 XrstorHelper(u64 context, u64 guest_address, u64 rfbm);
}  // namespace swift::x86

namespace swift::runtime::backend::arm64 {

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
    if (backedge_flags_recipe && backedge_flags_recipe->optimized &&
        backedge_flags_recipe->final_advance == inst) {
        // This is the last architectural boundary before the proven self/
        // single-cold terminal. Keep the requested NZCV live for the self
        // edge; both cold exits materialize it in their veneers.
        flag_state.flags_set = ir::Flags::None;
        flag_state.flags_clear = ir::Flags::None;
        return;
    }
    if (!FlagsRegsEnabled()) {
        MergeNZCV(FlagsRegsAuditMergeCause::AdvancePC,
                  flags_audit_block_edge);
    }
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
    // An inline-L1 return does not consume an RSB frame, so its calls must not
    // leave stale frames behind for modules that retain the RSB fallback.
    if (context.GetFeatures().indirect_l1 ||
        False(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
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

void JitTranslator::EmitCallReturn(ir::Inst* inst) {
    call_return_value = inst->GetArg<ir::Value>(0);
    call_return_pc = inst->GetArg<ir::Imm>(1).Get();
}

void JitTranslator::PrepareHostCallThunks(const std::vector<ir::Block*>& blocks) {
    host_call_thunks.clear();
    for (auto* block : blocks) {
        for (auto& inst : block->GetInstList()) {
            VAddr target{};
            if (inst.GetOp() == ir::OpCode::CallLambda) {
                const auto lambda = inst.GetArg<ir::Lambda>(0);
                if (!lambda.IsValue()) {
                    target = lambda.GetImm().Get();
                }
            } else if (inst.GetOp() == ir::OpCode::Sse42Str) {
                target = swift::x86::Sse42StrVectorHelperAddress(
                        u8(inst.GetArg<ir::Imm>(2).Get()));
            }
            if (target) {
                ++host_call_thunks[target].uses;
            }
        }
    }
    std::erase_if(host_call_thunks, [](const auto& item) {
        return item.second.uses < 2;
    });
    for (auto& item : host_call_thunks) {
        item.second.entry = std::make_unique<Label>();
    }
}

void JitTranslator::MaterializeHostCallTarget(u64 target) {
    // Keep the target ASLR-independent in size and visible to disk-cache relocation.
    __ movz(ip, target & 0xFFFFu, 0);
    __ movk(ip, (target >> 16) & 0xFFFFu, 16);
    __ movk(ip, (target >> 32) & 0xFFFFu, 32);
    __ movk(ip, (target >> 48) & 0xFFFFu, 48);
}

bool JitTranslator::TryEmitSharedHostCall(const ir::Lambda& lambda) {
    if (lambda.IsValue()) {
        return false;
    }
    const auto it = host_call_thunks.find(lambda.GetImm().Get());
    if (it == host_call_thunks.end()) {
        return false;
    }
    __ Bl(it->second.entry.get());
    return true;
}

bool JitTranslator::TryEmitSharedHostBranch(const ir::Lambda& lambda) {
    if (lambda.IsValue()) {
        return false;
    }
    const auto it = host_call_thunks.find(lambda.GetImm().Get());
    if (it == host_call_thunks.end()) {
        return false;
    }
    __ B(it->second.entry.get());
    return true;
}

void JitTranslator::EmitHostCallThunks() {
    for (auto& [target, thunk] : host_call_thunks) {
        __ Bind(thunk.entry.get());
        MaterializeHostCallTarget(target);
        __ Br(ip);
    }
    host_call_thunks.clear();
}

void JitTranslator::EmitHostCall(const ir::Lambda& lambda,
                                 const std::vector<ir::DataClass>& args,
                                 bool has_result,
                                 const Register& result,
                                 std::optional<Register> secondary_result) {
    ASSERT(args.size() <= 8);
    const auto helper = HelperCallContract::Resolve(lambda, context.GetFeatures());
    if (!helper.RetainsPendingNZCV()) {
        MergeNZCV(FlagsRegsAuditMergeCause::Helper,
                  FlagsRegsAuditEdgeKind::Host);
        FlushFlags();
    }

    // Materialize value arguments before taking the register capture. In
    // function mode an argument can be RegAlloc::MEM; context.X() then reloads
    // it into a caller-saved scratch register. If that reload happens after
    // the capture and argument setup subsequently reads the register's saved
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

    const bool preserve_all_leaf = helper.PreserveAllLeaf();
    const bool fpcr_transparent = helper.FPCRTransparent();
    const bool general_registers_only = helper.GeneralRegistersOnly();
    const bool preserves_pinned_state = helper.PreservesPinnedState();
    ASSERT(!preserves_pinned_state || args.size() <= 3);
    bool register_result =
            preserves_pinned_state && fpcr_transparent && has_result &&
            !secondary_result;

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
        // preserve_all keeps x9-x15. x9 therefore needs no capture unless it
        // is an argument source; x0-x8 remain caller-saved.
        const u32 last_clobbered = preserves_pinned_state
                ? 2
                : (preserve_all_leaf ? 8 : 9);
        if (!desc.is_float && desc.reg <= last_clobbered) {
            live_gprs.Mark(desc.reg);
        }
    }
    if (preserves_pinned_state) {
        live_gprs.Mark(ip.GetCode());
    }

    boost::container::small_vector<u32, 18> save_gprs;
    for (u32 code = 0; code <= 17; ++code) {
        if (live_gprs.Get(code) && helper.RequiresGPRCapture(code, argument_gprs.Get(code))) {
            save_gprs.push_back(code);
        }
    }
    if (register_result) {
        for (u32 code : save_gprs) {
            if (code == ip0.GetCode()) {
                register_result = false;
                break;
            }
        }
    }
    boost::container::small_vector<u32, 32> save_fprs;
    if (!general_registers_only) {
        FPRSMask live_fprs = context.GetLiveFPRs();
        for (const auto& desc : context.GetConfig().buffers_static_alloc) {
            if (desc.is_float) {
                live_fprs.Mark(desc.reg);
            }
        }
        for (u32 code = 0; code < 32; ++code) {
            if (live_fprs.Get(code) && helper.RequiresFPRCapture(code)) {
                save_fprs.push_back(code);
            }
        }
    }

    if (RAShapeProfEnabled()) {
        const auto abi = lambda.IsValue()
                ? RAShapeHelperABI::IndirectAAPCS
                : (preserve_all_leaf
                           ? RAShapeHelperABI::DirectPreserveAll
                           : RAShapeHelperABI::DirectAAPCS);
        RAShapeHelperCounters call{};
        call.calls = 1;
        // Save+restore of the live caller-saved GPR/FPR sets, plus the
        // explicit x29/x30 link pair. Argument loads and the result slot are
        // call plumbing, not caller-state captures, and stay out of this
        // counter by definition.
        call.capture_instructions =
                2 * ((save_gprs.size() + 1) / 2 +
                     (save_fprs.size() + 1) / 2 + 1);
        call.capture_code_bytes = call.capture_instructions * 4;
        call.capture_memory_bytes =
                2 * (save_gprs.size() * sizeof(u64) +
                     save_fprs.size() * sizeof(u128) + 2 * sizeof(u64));
        auto& total = context.GetRAShapeCounters().helpers[static_cast<size_t>(abi)];
        total.calls += call.calls;
        total.capture_instructions += call.capture_instructions;
        total.capture_code_bytes += call.capture_code_bytes;
        total.capture_memory_bytes += call.capture_memory_bytes;
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
    if (!register_result) {
        cursor += 8;
    }
    const u32 kSecondaryResultSlot = cursor;
    if (secondary_result) {
        cursor += 8;
    }
    // sp must stay 16-byte aligned, and the Q accesses below want a 16-byte
    // multiple as their base.
    const u32 kSimdSaveOffset = (cursor + 15u) & ~15u;
    const u32 kCapturesaveBytes =
            (kSimdSaveOffset + u32(save_fprs.size()) * 16u + 15u) & ~15u;
    const u32 kSaveBytes = kCapturesaveBytes;
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
            const bool source_requires_slot = helper.ArgumentRequiresSlot(src.GetCode());
            if (source_requires_slot) {
                __ Ldr(dst, MemOperand(sp, saved_offset(src.GetCode())));
            } else {
                __ Mov(dst, src);
            }
        }
    }
    // Conservative helpers execute under the caller's native FP environment.
    // A direct call site may explicitly certify FPCR transparency; only that
    // narrow allowlist keeps the AFP guest FPCR installed across BLR. The
    // runtime-entry frame sits immediately above this helper frame.
    if (sse_afp_nan && !fpcr_transparent) {
        __ Ldr(ip0,
               MemOperand(sp, kSaveBytes + kSseAFPHostFPCROffset));
        __ Msr(FPCR, ip0);
    }

    // Function address.
    if (lambda.IsValue()) {
        auto fn = *lambda_value;
        if (fn.GetCode() <= 17) {
            __ Ldr(ip, MemOperand(sp, saved_offset(fn.GetCode())));
        } else {
            __ Mov(ip, fn);
        }
        __ Blr(ip);
    } else {
        if (!TryEmitSharedHostCall(lambda)) {
            MaterializeHostCallTarget(lambda.GetImm().Get());
            __ Blr(ip);
        }
    }

    if (register_result) {
        __ Mov(ip0, x0);
    } else {
        __ Str(x0, MemOperand(sp, kResultSlot));
    }
    if (secondary_result) {
        __ Str(x1, MemOperand(sp, kSecondaryResultSlot));
    }
    if (sse_afp_nan && !fpcr_transparent) {
        // A helper such as XRSTOR or a NaN cold handler may have updated
        // context.mxcsr. Every helper takes the same compare path; none is
        // trusted through a target-specific cleanliness exemption.
        EmitSseAFPRestoreGuestFPCRCached(
                masm, state, kSaveBytes, ip, ip0, ip1);
    }

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
        if (register_result) {
            __ Mov(result, ip0);
        } else {
            __ Ldr(result, MemOperand(sp, kResultSlot));
        }
    }
    if (secondary_result) {
        __ Ldr(*secondary_result, MemOperand(sp, kSecondaryResultSlot));
    }
    __ Add(sp, sp, kSaveBytes);
    if (sync_xmm_after) {
        RestoreStaticFPRUniforms();
    }
}

void JitTranslator::EmitPreserveAllPairCall(ir::Inst* inst,
                                            VAddr target,
                                            const std::vector<ir::DataClass>& args,
                                            ir::OpCode secondary,
                                            ir::HostRegisterEffect host_registers) {
    ASSERT(args.size() == 3);
    ASSERT(host_registers == ir::HostRegisterEffect::GeneralOnly);
    const auto secondary_results = inst->GetPseudoOperations(secondary);
    ASSERT(secondary_results.size() <= 1);
    MergeNZCV(FlagsRegsAuditMergeCause::Helper,
              FlagsRegsAuditEdgeKind::Host);
    FlushFlags();

    std::array<std::optional<XRegister>, 3> value_args;
    for (u32 index = 0; index < args.size(); ++index) {
        if (args[index].IsValue()) {
            value_args[index] = context.X(args[index].value);
        }
    }
    const auto primary_result = context.R(ir::Value{inst});
    const auto secondary_result = secondary_results.empty()
            ? std::optional<Register>{}
            : std::optional<Register>{
                      context.RForWrite(ir::Value{secondary_results.front()})};

    __ Sub(sp, sp, PairCallFrame::Size);
    __ Stp(x30, ip, MemOperand(sp, PairCallFrame::Link));
    __ Str(ip0, MemOperand(sp, PairCallFrame::SavedX16));
    for (u32 index = 0; index < args.size(); ++index) {
        if (value_args[index]) {
            __ Str(*value_args[index],
                   MemOperand(sp, PairCallFrame::Argument(index)));
        }
    }
    for (u32 index = 0; index < args.size(); ++index) {
        if (args[index].IsImm()) {
            __ Mov(ip, args[index].imm.Get());
            __ Str(ip, MemOperand(sp, PairCallFrame::Argument(index)));
        }
    }
    MaterializeHostCallTarget(target);
    const u64 trampoline = context.GetPairCallTrampoline();
    ASSERT(trampoline);
    __ movz(ip0, trampoline & 0xFFFFu, 0);
    __ movk(ip0, (trampoline >> 16) & 0xFFFFu, 16);
    __ movk(ip0, (trampoline >> 32) & 0xFFFFu, 32);
    __ movk(ip0, (trampoline >> 48) & 0xFFFFu, 48);
    __ Blr(ip0);
    __ Ldp(x30, ip, MemOperand(sp, PairCallFrame::Link));
    __ Ldr(ip0, MemOperand(sp, PairCallFrame::SavedX16));
    __ Ldr(primary_result,
           MemOperand(sp, PairCallFrame::PrimaryResult));
    if (secondary_result) {
        __ Ldr(*secondary_result,
               MemOperand(sp, PairCallFrame::SecondaryResult));
    }
    __ Add(sp, sp, PairCallFrame::Size);
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
    if (const auto address = MatchPinnedMemoryAddress(inst)) {
        if (address->offset == 0) {
            pinned_gprs.pinned_gpr_values.emplace(inst, address->target);
            return;
        }
        if (context.IsSpilled(ir::Value{inst})) {
            memory_state.spilled_memory_operands.emplace(
                    inst,
                    SpilledMemoryOperand{address->memory,
                                         XRegister(address->target),
                                         address->offset});
            return;
        }
    }
    auto operand = inst->GetArg<ir::Operand>(0);
    const auto pinned_result = ResolvePinnedGPRValue(ir::Value{inst});
    auto result = pinned_result ? Register{*pinned_result}
                                : context.R(ir::Value{inst});
    if (EmitCachedConstAddress(inst, result)) {
        return;
    }
    if (memory_state.abs_const_mat && operand.GetRight().Null() && operand.GetLeft().IsImm()) {
        __ Mov(result, operand.GetLeft().imm.Get());
        return;
    }
    if ((memory_state.mem_narrow_fuse || memory_state.addr_ea_tie) && operand.GetRight().Null() &&
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
    if (!memory_state.mem_narrow_fuse || operand.GetRight().Null()) {
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
        const auto value = operand.GetLeft().value;
        const auto pinned = ResolvePinnedGPRValue(value);
        left = pinned ? Register{*pinned} : context.R(value, true);
    }
    Register dst = left.Is64Bits() ? result.X() : result.W();
    auto right = operand.GetRight();
    if (right.IsImm()) {
        const auto imm = right.imm.GetSigned();
        if (operand.GetOp() == ir::OperandOp::Plus) {
            if (__ IsImmAddSub(imm)) {
                __ Add(dst, left, imm);
            } else if (imm < 0 && imm != std::numeric_limits<s64>::min() &&
                       __ IsImmAddSub(-imm)) {
                __ Sub(dst, left, -imm);
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

    const auto pinned = ResolvePinnedGPRValue(right.value);
    auto right_reg = pinned ? Register{*pinned}
                            : context.R(right.value, true);
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

void JitTranslator::PublishPendingStaticLocation() {
    if (!static_next_loc) {
        return;
    }
    __ Mov(ip, *static_next_loc);
    __ Str(ip, MemOperand(state, state_offset_current_loc));
    static_next_loc.reset();
}

void JitTranslator::EmitSetLocation(ir::Inst* inst) {
    auto location = inst->GetArg<ir::Lambda>(0);
    // Any SetLocation invalidates an earlier constant, including a dynamic one:
    // SetLocation is *not* only emitted just before a terminal (decoder_x87.cc
    // and decoder_xsave.cc place the faulting PC mid-block), so a block can
    // hold a constant SetLocation followed by the `jmp *rax` one -- and reusing
    // the stale constant there would turn an indirect jump into a jump back
    // into the middle of the same block.
    static_next_loc.reset();
    dynamic_next_loc.reset();
    dynamic_location_miss = nullptr;
    if (location.IsValue()) {
        const auto target = context.X(location.GetValue());
        if (!terminal_location_publication.Defers(inst)) {
            __ Str(target, MemOperand(state, state_offset_current_loc));
        } else {
            dynamic_location_miss =
                    terminal_location_publication.GetOrCreateMissLabel(target);
        }
        dynamic_next_loc = location.GetValue();
    } else {
        static_next_loc = location.GetImm().Get();
        if (inst != terminal_body_inst) {
            PublishPendingStaticLocation();
        }
    }
}

void JitTranslator::EmitCheckMemoryAlignment(ir::Inst* inst) {
    const auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto mask = inst->GetArg<ir::Imm>(1).Get();
    Label aligned;

    // Tst clobbers NZCV, so commit any pending guest flags first. On failure,
    // Ret returns to the runtime-entry dispatcher, which observes PageFatal in
    // State::halt_reason and exits through the normal guest-fault path.
    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    __ Tst(address, mask);
    __ B(&aligned, eq);
    __ Mov(ipw, static_cast<u32>(HaltReason::PageFatal));
    __ Str(ipw, MemOperand(state, state_offset_halt_reason));
    context.ReturnHost();
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

void JitTranslator::EmitNop(ir::Inst* inst) {}

}  // namespace swift::runtime::backend::arm64
