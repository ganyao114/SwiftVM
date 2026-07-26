#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/frontend/x86/x87.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitAdvancePC(ir::Inst* inst) {
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
    __ Cbz(context.W(cond), GetLocalLabel(inst));
}

void JitTranslator::EmitGoto(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
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
    // Argument registers are unioned in explicitly instead of trusting the
    // mask to contain them.  The argument setup below reads each one back from
    // its save slot (loading x0 first would otherwise clobber a later
    // argument's source register), so a register with no slot would silently
    // pass garbage to the helper -- the worst failure shape available here.
    for (const auto& reg : value_args) {
        if (reg.GetCode() <= 17) {
            live_gprs.Mark(reg.GetCode());
        }
    }
    if (lambda_value && lambda_value->GetCode() <= 17) {
        live_gprs.Mark(lambda_value->GetCode());
    }

    boost::container::small_vector<u32, 18> save_gprs;
    for (u32 code = 0; code <= 17; ++code) {
        if (live_gprs.Get(code)) {
            save_gprs.push_back(code);
        }
    }
    boost::container::small_vector<u32, 32> save_fprs;
    const FPRSMask& live_fprs = context.GetLiveFPRs();
    for (u32 code = 0; code < 32; ++code) {
        if (live_fprs.Get(code)) {
            save_fprs.push_back(code);
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
    const u32 kSaveBytes =
            (kSimdSaveOffset + u32(save_fprs.size()) * 16u + 15u) & ~15u;
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

    // Function address.
    if (lambda.IsValue()) {
        auto fn = *lambda_value;
        if (fn.GetCode() <= 17) {
            __ Ldr(ip, MemOperand(sp, saved_offset(fn.GetCode())));
        } else {
            __ Mov(ip, fn);
        }
    } else {
        __ Mov(ip, lambda.GetImm().Get());
    }
    __ Blr(ip);

    __ Str(x0, MemOperand(sp, kResultSlot));

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
    __ Mov(result, EmitOperand(operand));
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
    if (location.IsValue()) {
        __ Str(context.X(location.GetValue()), MemOperand(state, state_offset_current_loc));
    } else {
        __ Mov(ip, location.GetImm().Get());
        __ Str(ip, MemOperand(state, state_offset_current_loc));
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
