#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"

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

    // Save all potentially allocated caller-saved GPRs (x0-x10, x12-x17) plus
    // x29/x30: the Blr below clobbers the link register holding this block's
    // return address back to the dispatcher.
    // ip (x11) is reserved scratch; x18 is reserved on Apple; x19+ are callee-saved.
    //
    // Preserve every SIMD register as a full 128-bit value as well. The host
    // ABI leaves v0-v7 and v16-v31 caller-saved, and only guarantees the low
    // 64 bits of v8-v15. The function-wide allocator can keep a V128 live
    // across CallLambda, so the ABI's partial preservation is insufficient.
    constexpr u32 kGprSaveBytes = 16 * 10;
    constexpr u32 kSimdSaveOffset = kGprSaveBytes;
    constexpr u32 kSimdSaveBytes = 32 * 16;
    constexpr u32 kSaveBytes = kGprSaveBytes + kSimdSaveBytes;
    auto saved_offset = [](u32 code) -> u32 {
        if (code <= 10) {
            return code * 8;
        }
        switch (code) {
            case 12:
                return 88;
            case 13:
                return 96;
            case 14:
                return 104;
            case 15:
                return 112;
            case 16:
                return 120;
            case 17:
                return 128;
            default:
                PANIC();
        }
    };
    constexpr u32 kResultSlot = 136;

    __ Sub(sp, sp, kSaveBytes);
    __ Stp(x0, x1, MemOperand(sp, 0));
    __ Stp(x2, x3, MemOperand(sp, 16));
    __ Stp(x4, x5, MemOperand(sp, 32));
    __ Stp(x6, x7, MemOperand(sp, 48));
    __ Stp(x8, x9, MemOperand(sp, 64));
    __ Stp(x10, x12, MemOperand(sp, 80));
    __ Stp(x13, x14, MemOperand(sp, 96));
    __ Stp(x15, x16, MemOperand(sp, 112));
    __ Str(x17, MemOperand(sp, 128));
    __ Stp(x29, x30, MemOperand(sp, 144));
    for (u32 code = 0; code < 32; ++code) {
        __ Str(VRegister::GetQRegFromCode(code),
               MemOperand(sp, kSimdSaveOffset + code * 16));
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

    for (u32 code = 0; code < 32; ++code) {
        __ Ldr(VRegister::GetQRegFromCode(code),
               MemOperand(sp, kSimdSaveOffset + code * 16));
    }
    __ Ldp(x0, x1, MemOperand(sp, 0));
    __ Ldp(x2, x3, MemOperand(sp, 16));
    __ Ldp(x4, x5, MemOperand(sp, 32));
    __ Ldp(x6, x7, MemOperand(sp, 48));
    __ Ldp(x8, x9, MemOperand(sp, 64));
    __ Ldp(x10, x12, MemOperand(sp, 80));
    __ Ldp(x13, x14, MemOperand(sp, 96));
    __ Ldp(x15, x16, MemOperand(sp, 112));
    __ Ldr(x17, MemOperand(sp, 128));
    __ Ldp(x29, x30, MemOperand(sp, 144));
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
