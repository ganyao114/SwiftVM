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

void JitTranslator::EmitX87Op(ir::Inst* inst) {
    const auto context_value = inst->GetArg<ir::Value>(0);
    const auto command = inst->GetArg<ir::Imm>(1);
    const auto address = inst->GetArg<ir::Value>(2);
    const auto self = ir::Value{inst};
    const bool has_result = context.HasAllocation(self);
    Register result{};
    if (has_result) {
        result = context.R(self);
    }

    // X87Op mutates the uniform buffer directly. Resolve pending guest flags
    // before any inline path, and before a possible helper branch, so the
    // translator's compile-time flag state is identical on every path.
    MergeNZCV();
    FlushFlags();

    constexpr u32 kContextBase = state_offset_uniform_buffer;
    constexpr u32 kFcw = kContextBase + offsetof(swift::x86::ThreadContext64, x87_fcw);
    constexpr u32 kFsw = kContextBase + offsetof(swift::x86::ThreadContext64, x87_fsw);
    constexpr u32 kFtw = kContextBase + offsetof(swift::x86::ThreadContext64, x87_ftw);
    constexpr u32 kFop = kContextBase + offsetof(swift::x86::ThreadContext64, x87_fop);
    constexpr u32 kFip = kContextBase + offsetof(swift::x86::ThreadContext64, x87_fip);
    constexpr u32 kFdp = kContextBase + offsetof(swift::x86::ThreadContext64, x87_fdp);
    constexpr u32 kRegs = kContextBase + offsetof(swift::x86::ThreadContext64, x87_regs);
    constexpr u32 kReducedMarkerOffset =
            offsetof(swift::x86::X87Reg, reserved);
    constexpr u8 kReducedMarker = swift::x86::kX87ReducedMarker;
    constexpr u8 kReducedReadyMarker =
            swift::x86::kX87ReducedReadyMarker;

    const u64 command_word = command.Get();
    const auto action = static_cast<swift::x86::X87Action>(command_word & 0xFF);
    const auto format =
            static_cast<swift::x86::X87Format>((command_word >> 8) & 0xFF);
    const u8 index = static_cast<u8>((command_word >> 16) & 7);
    const u8 operation = static_cast<u8>((command_word >> 24) & 0xFF);
    const u32 command_flags = static_cast<u32>(command_word >> 32);

    auto zero_result = [&] {
        if (has_result) {
            __ Mov(result, 0);
        }
    };
    auto fallback = [&] {
        std::vector<ir::DataClass> args{
                ir::DataClass{context_value},
                ir::DataClass{command},
                ir::DataClass{address},
        };
        EmitHostCall(
                ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&swift::x86::X87Dispatch)}},
                args,
                has_result,
                result);
    };

    switch (action) {
        case swift::x86::X87Action::Init: {
            auto value = context.GetTmpX();
            __ Mov(value, 0x037F);
            __ Strh(value.W(), MemOperand(state, kFcw));
            __ Strh(wzr, MemOperand(state, kFsw));
            __ Mov(value, 0xFFFF);
            __ Strh(value.W(), MemOperand(state, kFtw));
            __ Strh(wzr, MemOperand(state, kFop));
            __ Str(xzr, MemOperand(state, kFip));
            __ Str(xzr, MemOperand(state, kFdp));
            zero_result();
            return;
        }
        case swift::x86::X87Action::ClearExceptions: {
            auto fsw = context.GetTmpX();
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ And(fsw.W(), fsw.W(), 0x7F00);
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            return;
        }
        case swift::x86::X87Action::StoreControl: {
            auto value = context.GetTmpX();
            __ Ldrh(value.W(), MemOperand(state, kFcw));
            auto guest = context.X(address);
            __ Strh(value.W(),
                    use_memory_base ? BiasMem(guest) : MemOperand(guest));
            zero_result();
            return;
        }
        case swift::x86::X87Action::LoadControl: {
            auto fcw = context.GetTmpX();
            auto fsw = context.GetTmpX();
            auto pending = context.GetTmpX();
            auto guest = context.X(address);
            __ Ldrh(fcw.W(),
                    use_memory_base ? BiasMem(guest) : MemOperand(guest));
            __ Strh(fcw.W(), MemOperand(state, kFcw));
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ And(pending.W(), fsw.W(), 0x3F);
            __ Bic(pending.W(), pending.W(), fcw.W());
            Label no_pending;
            Label summary_done;
            __ Cbz(pending.W(), &no_pending);
            __ Orr(fsw.W(), fsw.W(), 0x8080);
            __ B(&summary_done);
            __ Bind(&no_pending);
            __ And(fsw.W(), fsw.W(), 0x7F7F);
            __ Bind(&summary_done);
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            return;
        }
        case swift::x86::X87Action::StoreStatus: {
            auto fsw = context.GetTmpX();
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            if (format == swift::x86::X87Format::Register) {
                ASSERT(has_result);
                __ Mov(result, fsw);
            } else {
                auto guest = context.X(address);
                __ Strh(fsw.W(),
                        use_memory_base ? BiasMem(guest) : MemOperand(guest));
                zero_result();
            }
            return;
        }
        case swift::x86::X87Action::LoadFloat: {
            if (format != swift::x86::X87Format::Float80) {
                break;
            }
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto top = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto tag = context.GetTmpX();
            auto significand = context.GetTmpX();
            auto sign_exp = context.GetTmpX();
            auto reg_address = context.GetTmpX();
            auto guest = context.X(address);
            Label slow;
            Label tag_special;
            Label tag_zero;
            Label tag_valid;
            Label tag_ready;
            Label done;
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(top.W(), fsw.W(), 11, 3);
            __ Add(top.W(), top.W(), 7);
            __ And(top.W(), top.W(), 7);
            __ Lsl(shift.W(), top.W(), 1);
            __ Lsr(tag.W(), ftw.W(), shift.W());
            __ And(tag.W(), tag.W(), 3);
            __ Cmp(tag.W(), 3);
            __ B(ne, &slow);
            __ Ldr(significand,
                   use_memory_base ? BiasMem(guest) : MemOperand(guest));
            __ Ldrh(sign_exp.W(),
                    use_memory_base ? BiasMem(guest, s64{8}) : MemOperand(guest, 8));
            __ Add(reg_address, state, kRegs);
            __ Add(reg_address, reg_address, Operand{top, LSL, 4});
            __ Str(significand, MemOperand(reg_address));
            __ Strh(sign_exp.W(), MemOperand(reg_address, 8));
            __ Str(wzr, MemOperand(reg_address, 10));
            __ Strh(wzr, MemOperand(reg_address, 14));

            __ And(tag.W(), sign_exp.W(), 0x7FFF);
            __ Cmp(tag.W(), 0x7FFF);
            __ B(eq, &tag_special);
            __ Cbnz(tag.W(), &tag_valid);
            __ Cbz(significand, &tag_zero);
            __ Bind(&tag_special);
            __ Mov(tag.W(), 2);
            __ B(&tag_ready);
            __ Bind(&tag_zero);
            __ Mov(tag.W(), 1);
            __ B(&tag_ready);
            __ Bind(&tag_valid);
            __ Mov(tag.W(), 0);
            __ Bind(&tag_ready);
            __ Mov(reg_address.W(), 3);
            __ Lsl(reg_address.W(), reg_address.W(), shift.W());
            __ Bic(ftw.W(), ftw.W(), reg_address.W());
            __ Lsl(tag.W(), tag.W(), shift.W());
            __ Orr(ftw.W(), ftw.W(), tag.W());
            __ And(fsw.W(), fsw.W(), 0xC5FF);
            __ Orr(fsw.W(), fsw.W(), Operand{top.W(), LSL, 11});
            __ Strh(ftw.W(), MemOperand(state, kFtw));
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();
            __ Bind(&done);
            return;
        }
        case swift::x86::X87Action::StoreFloat: {
            if (format != swift::x86::X87Format::Float80) {
                break;
            }
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto top = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto tag = context.GetTmpX();
            auto significand = context.GetTmpX();
            auto sign_exp = context.GetTmpX();
            auto reg_address = context.GetTmpX();
            auto guest = context.X(address);
            Label slow;
            Label done;
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(top.W(), fsw.W(), 11, 3);
            __ Lsl(shift.W(), top.W(), 1);
            __ Lsr(tag.W(), ftw.W(), shift.W());
            __ And(tag.W(), tag.W(), 3);
            __ Cmp(tag.W(), 3);
            __ B(eq, &slow);
            __ Add(reg_address, state, kRegs);
            __ Add(reg_address, reg_address, Operand{top, LSL, 4});
            __ Ldr(significand, MemOperand(reg_address));
            __ Ldrh(sign_exp.W(), MemOperand(reg_address, 8));
            __ Str(significand,
                   use_memory_base ? BiasMem(guest) : MemOperand(guest));
            __ Strh(sign_exp.W(),
                    use_memory_base ? BiasMem(guest, s64{8}) : MemOperand(guest, 8));
            __ And(fsw.W(), fsw.W(), 0xFDFF);
            if (command_flags & swift::x86::X87Pop) {
                __ Mov(tag.W(), 3);
                __ Lsl(tag.W(), tag.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), tag.W());
                __ Add(top.W(), top.W(), 1);
                __ And(top.W(), top.W(), 7);
                __ And(fsw.W(), fsw.W(), 0xC5FF);
                __ Orr(fsw.W(), fsw.W(), Operand{top.W(), LSL, 11});
                __ Strh(ftw.W(), MemOperand(state, kFtw));
            }
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();
            __ Bind(&done);
            return;
        }
        case swift::x86::X87Action::Binary: {
            // Reduced-precision register arithmetic.  The architectural state
            // remains ext80; only operands whose ext80 encoding is exactly a
            // normal binary64 value take this path.  Everything involving
            // memory, an empty/special stack value, a non-RNE control word, or
            // precision below PC=64 falls back to the bit-exact SoftFloat
            // implementation.
            if (format != swift::x86::X87Format::Register) {
                break;
            }

            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto fcw = context.GetTmpX();
            auto left_physical = context.GetTmpX();
            auto right_physical = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto left_address = context.GetTmpX();
            auto right_address = context.GetTmpX();
            auto left_bits = context.GetTmpX();
            auto right_bits = context.GetTmpX();
            auto round_bits = context.GetTmpX();
            auto left_fp = context.GetTmpV();
            auto right_fp = context.GetTmpV();
            Label slow;
            Label done;

            __ Ldrh(fcw.W(), MemOperand(state, kFcw));
            __ And(scratch.W(), fcw.W(), 0x0F00);
            __ Cmp(scratch.W(), 0x0300);  // PC=64 and RC=nearest-even.
            __ B(ne, &slow);

            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(left_physical.W(), fsw.W(), 11, 3);
            if (command_flags & swift::x86::X87DestIndex) {
                __ Add(left_physical.W(), left_physical.W(), index);
                __ And(left_physical.W(), left_physical.W(), 7);
                __ Ubfx(right_physical.W(), fsw.W(), 11, 3);
            } else {
                __ Add(right_physical.W(), left_physical.W(), index);
                __ And(right_physical.W(), right_physical.W(), 7);
            }

            // Both source tags must be non-empty.  Special values deliberately
            // stay on SoftFloat even when their payload happens to fit f64.
            for (auto physical : {left_physical, right_physical}) {
                __ Lsl(scratch.W(), physical.W(), 1);
                __ Lsr(scratch.W(), ftw.W(), scratch.W());
                __ And(scratch.W(), scratch.W(), 3);
                __ Cmp(scratch.W(), 3);
                __ B(eq, &slow);
            }

            __ Add(left_address, state, kRegs);
            __ Add(left_address,
                   left_address,
                   Operand{left_physical, LSL, 4});
            __ Add(right_address, state, kRegs);
            __ Add(right_address,
                   right_address,
                   Operand{right_physical, LSL, 4});

            // Spell out the two conversions rather than retaining a label in
            // a lambda: VIXL labels must have stable storage.
            auto convert_one = [&](const Register& ext_address,
                                   const Register& bits,
                                   const Register& exponent,
                                   const VRegister& fp) {
                auto sign_exp = scratch;
                Label marker_ready;
                Label input_zero;
                Label round_increment;
                Label round_ready;
                Label exponent_ready;
                Label converted;
                __ Ldrb(round_bits.W(),
                        MemOperand(ext_address, kReducedMarkerOffset));
                __ Cmp(round_bits.W(), kReducedMarker);
                __ B(eq, &marker_ready);
                __ Cmp(round_bits.W(), kReducedReadyMarker);
                __ B(ne, &slow);
                __ Bind(&marker_ready);
                __ Ldr(bits, MemOperand(ext_address));
                __ Ldrh(sign_exp.W(), MemOperand(ext_address, 8));
                __ And(exponent.W(), sign_exp.W(), 0x7FFF);
                __ Cbz(exponent.W(), &input_zero);
                __ Cmp(exponent.W(), 0x3C01);
                __ B(lt, &slow);
                __ Cmp(exponent.W(), 0x43FE);
                __ B(gt, &slow);
                __ Tst(bits, 0x8000000000000000ull);
                __ B(eq, &slow);

                // Canonical marker values must already have the eleven ext80
                // tail bits clear.  Runtime-revalidated helper results retain
                // those bits architecturally and are rounded to nearest-even
                // only when a later reduced operation actually consumes them.
                __ And(exponent, bits, 0x7FF);
                __ Lsr(bits, bits, 11);
                __ Cmp(round_bits.W(), kReducedMarker);
                __ B(ne, &round_ready);
                __ Cbnz(exponent, &slow);
                __ B(&exponent_ready);

                __ Bind(&round_ready);
                __ Cmp(exponent, 0x400);
                __ B(gt, &round_increment);
                __ B(lt, &exponent_ready);
                __ Tbz(bits, 0, &exponent_ready);
                __ Bind(&round_increment);
                __ Add(bits, bits, 1);

                __ Bind(&exponent_ready);
                __ And(exponent.W(), sign_exp.W(), 0x7FFF);
                __ Sub(exponent.W(), exponent.W(), 0x3C00);
                // Rounding 1.111... may carry in to a new exponent.
                Label no_round_carry;
                __ Tbz(bits, 53, &no_round_carry);
                __ Lsr(bits, bits, 1);
                __ Add(exponent.W(), exponent.W(), 1);
                __ Bind(&no_round_carry);
                __ Lsl(exponent, exponent, 52);
                __ And(bits, bits, 0x000FFFFFFFFFFFFFull);
                __ Orr(bits, bits, exponent);
                __ Ubfx(sign_exp, sign_exp, 15, 1);
                __ Orr(bits, bits, Operand{sign_exp, LSL, 63});
                __ Fmov(fp.D(), bits);
                __ B(&converted);

                __ Bind(&input_zero);
                __ Cbnz(bits, &slow);
                __ Ubfx(bits, sign_exp, 15, 1);
                __ Lsl(bits, bits, 63);
                __ Fmov(fp.D(), bits);
                __ Bind(&converted);
            };

            convert_one(left_address, left_bits, right_bits, left_fp);
            convert_one(right_address, right_bits, left_bits, right_fp);

            if (command_flags & swift::x86::X87Reverse) {
                std::swap(left_fp, right_fp);
            }

            __ Msr(FPSR, xzr);
            switch (static_cast<swift::x86::X87Binary>(operation)) {
                case swift::x86::X87Binary::Add:
                    __ Fadd(left_fp.D(), left_fp.D(), right_fp.D());
                    break;
                case swift::x86::X87Binary::Mul:
                    __ Fmul(left_fp.D(), left_fp.D(), right_fp.D());
                    break;
                case swift::x86::X87Binary::Sub:
                    __ Fsub(left_fp.D(), left_fp.D(), right_fp.D());
                    break;
                case swift::x86::X87Binary::Div:
                    __ Fdiv(left_fp.D(), left_fp.D(), right_fp.D());
                    break;
            }
            __ Mrs(fcw, FPSR);

            // A binary64 add/sub may discard low bits that the 64-bit ext80
            // significand would retain.  The marked operands only prove that
            // conversion to f64 is lossless; they do not prove that the
            // arithmetic result is.  Bail out before publishing any state
            // when ARM reports an inexact add/sub, and let SoftFloat compute
            // the architectural ext80 result.  Exact f64 add/sub operations
            // remain on the native path.  Mul/div deliberately retain the
            // opt-in reduced-precision behavior documented by the probe.
            if (static_cast<swift::x86::X87Binary>(operation) ==
                        swift::x86::X87Binary::Add ||
                static_cast<swift::x86::X87Binary>(operation) ==
                        swift::x86::X87Binary::Sub) {
                __ Tbnz(fcw, 4, &slow);  // FPSR.IXC
            }
            __ Fmov(left_bits, left_fp.D());

            // Convert the binary64 result back to an exact ext80 encoding.
            // Reduced-mode overflow therefore becomes ext80 infinity and
            // reduced-mode underflow remains the corresponding f64 subnormal.
            auto exponent = right_bits;
            auto sign_exp = scratch;
            auto significand = left_bits;
            auto shift = right_address;
            Label result_subnormal;
            Label result_zero;
            Label result_special;
            Label result_ready;
            __ Ubfx(exponent, left_bits, 52, 11);
            __ Ubfx(sign_exp, left_bits, 63, 1);
            __ Lsl(sign_exp, sign_exp, 15);
            __ And(significand, left_bits, 0x000FFFFFFFFFFFFFull);
            __ Cbz(exponent, &result_subnormal);
            __ Cmp(exponent, 0x7FF);
            __ B(eq, &result_special);
            __ Add(exponent, exponent, 0x3C00);
            __ Orr(sign_exp, sign_exp, exponent);
            __ Lsl(significand, significand, 11);
            __ Orr(significand, significand, 0x8000000000000000ull);
            __ B(&result_ready);
            __ Bind(&result_subnormal);
            __ Cbz(significand, &result_zero);
            __ Clz(shift, significand);
            __ Lsl(significand, significand, shift);
            __ Mov(exponent, 0x3C0C);
            __ Sub(exponent, exponent, shift);
            __ Orr(sign_exp, sign_exp, exponent);
            __ B(&result_ready);
            __ Bind(&result_zero);
            __ Mov(significand, 0);
            __ B(&result_ready);
            __ Bind(&result_special);
            __ Orr(sign_exp, sign_exp, 0x7FFF);
            __ Lsl(significand, significand, 11);
            __ Orr(significand, significand, 0x8000000000000000ull);
            __ Bind(&result_ready);
            __ Str(significand, MemOperand(left_address));
            __ Strh(sign_exp.W(), MemOperand(left_address, 8));
            __ Str(wzr, MemOperand(left_address, 10));
            __ Strh(wzr, MemOperand(left_address, 14));
            __ Mov(exponent.W(), kReducedMarker);
            __ Strb(exponent.W(),
                    MemOperand(left_address, kReducedMarkerOffset));

            // Reclassify the destination tag.
            __ Lsl(shift.W(), left_physical.W(), 1);
            __ Mov(exponent.W(), 3);
            __ Lsl(exponent.W(), exponent.W(), shift.W());
            __ Bic(ftw.W(), ftw.W(), exponent.W());
            Label tag_valid;
            Label tag_special;
            Label tag_done;
            __ And(exponent.W(), sign_exp.W(), 0x7FFF);
            __ Cmp(exponent.W(), 0x7FFF);
            __ B(eq, &tag_special);
            __ Cbnz(exponent.W(), &tag_valid);
            __ Cbnz(significand, &tag_special);
            __ Mov(exponent.W(), 1);  // zero
            __ B(&tag_done);
            __ Bind(&tag_special);
            __ Mov(exponent.W(), 2);
            __ B(&tag_done);
            __ Bind(&tag_valid);
            __ Mov(exponent.W(), 0);
            __ Bind(&tag_done);
            __ Lsl(exponent.W(), exponent.W(), shift.W());
            __ Orr(ftw.W(), ftw.W(), exponent.W());

            // Map ARM FPSR exception bits to the x87 sticky exception bits.
            auto exceptions = exponent;
            auto flag = shift;
            __ And(exceptions.W(), fcw.W(), 1);           // IOC -> IE
            __ Ubfx(flag.W(), fcw.W(), 1, 4);             // DZC..IXC
            __ Lsl(flag.W(), flag.W(), 2);                // -> ZE..PE
            __ Orr(exceptions.W(), exceptions.W(), flag.W());
            __ Ubfx(flag.W(), fcw.W(), 7, 1);             // IDC -> DE
            __ Orr(exceptions.W(), exceptions.W(), Operand{flag.W(), LSL, 1});
            __ Orr(fsw.W(), fsw.W(), exceptions.W());
            __ And(fsw.W(), fsw.W(), 0xFDFF);             // C1=0

            // Unmasked pending exceptions set ES and B.
            __ Ldrh(fcw.W(), MemOperand(state, kFcw));
            __ And(flag.W(), fsw.W(), 0x3F);
            __ Bic(flag.W(), flag.W(), fcw.W());
            Label no_pending;
            Label summary_done;
            __ Cbz(flag.W(), &no_pending);
            __ Orr(fsw.W(), fsw.W(), 0x8080);
            __ B(&summary_done);
            __ Bind(&no_pending);
            __ And(fsw.W(), fsw.W(), 0x7F7F);
            __ Bind(&summary_done);

            if (command_flags & swift::x86::X87Pop) {
                // Pop the old ST0 after writing ST(i).
                __ Ubfx(right_physical.W(), fsw.W(), 11, 3);
                __ Lsl(shift.W(), right_physical.W(), 1);
                __ Mov(exponent.W(), 3);
                __ Lsl(exponent.W(), exponent.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), exponent.W());
                __ Add(right_physical.W(), right_physical.W(), 1);
                __ And(right_physical.W(), right_physical.W(), 7);
                __ And(fsw.W(), fsw.W(), 0xC5FF);
                __ Orr(fsw.W(),
                       fsw.W(),
                       Operand{right_physical.W(), LSL, 11});
            }
            __ Strh(ftw.W(), MemOperand(state, kFtw));
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();

            // The exact helper clears reduced provenance.  Revalidate the
            // result at runtime so one bailout does not permanently poison a
            // register-arithmetic chain.  Pop forms leave their result at the
            // new ST(0); non-pop ST(i) destinations retain their logical
            // index.  The exact ext80 payload is never rewritten here.
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(left_physical.W(), fsw.W(), 11, 3);
            if (!(command_flags & swift::x86::X87Pop) &&
                (command_flags & swift::x86::X87DestIndex)) {
                __ Add(left_physical.W(), left_physical.W(), index);
                __ And(left_physical.W(), left_physical.W(), 7);
            }
            __ Lsl(right_physical.W(), left_physical.W(), 1);
            __ Lsr(scratch.W(), ftw.W(), right_physical.W());
            __ And(scratch.W(), scratch.W(), 3);
            Label revalidate_done;
            Label revalidate_zero;
            Label revalidate_canonical;
            Label revalidate_ready;
            __ Cmp(scratch.W(), 3);
            __ B(eq, &revalidate_done);
            __ Add(left_address, state, kRegs);
            __ Add(left_address,
                   left_address,
                   Operand{left_physical, LSL, 4});
            __ Ldr(left_bits, MemOperand(left_address));
            __ Ldrh(scratch.W(), MemOperand(left_address, 8));
            __ And(right_bits.W(), scratch.W(), 0x7FFF);
            __ Cbz(right_bits.W(), &revalidate_zero);
            __ Cmp(right_bits.W(), 0x3C01);
            __ B(lt, &revalidate_done);
            __ Cmp(right_bits.W(), 0x43FE);
            __ B(gt, &revalidate_done);
            __ Tst(left_bits, 0x8000000000000000ull);
            __ B(eq, &revalidate_done);
            __ And(right_address, left_bits, 0x7FF);
            __ Cbz(right_address, &revalidate_canonical);
            // A non-canonical result at the top binary64 exponent could round
            // to infinity; keep that edge on the exact helper chain.
            __ Cmp(right_bits.W(), 0x43FE);
            __ B(eq, &revalidate_done);
            __ B(&revalidate_ready);
            __ Bind(&revalidate_zero);
            __ Cbnz(left_bits, &revalidate_done);
            __ Bind(&revalidate_canonical);
            __ Mov(right_bits.W(), kReducedMarker);
            __ Strb(right_bits.W(),
                    MemOperand(left_address, kReducedMarkerOffset));
            __ B(&revalidate_done);
            __ Bind(&revalidate_ready);
            __ Mov(right_bits.W(), kReducedReadyMarker);
            __ Strb(right_bits.W(),
                    MemOperand(left_address, kReducedMarkerOffset));
            __ Bind(&revalidate_done);
            __ Bind(&done);
            return;
        }
        case swift::x86::X87Action::AdjustTop: {
            auto fsw = context.GetTmpX();
            auto top = context.GetTmpX();
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ubfx(top.W(), fsw.W(), 11, 3);
            __ Add(top.W(),
                   top.W(),
                   (command_flags & swift::x86::X87IncrementTop) ? 1 : 7);
            __ And(top.W(), top.W(), 7);
            // Replace TOP and clear C1, but preserve C0/C2/C3 and all
            // accumulated exception state.
            __ And(fsw.W(), fsw.W(), 0xC5FF);
            __ Orr(fsw.W(), fsw.W(), Operand{top.W(), LSL, 11});
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            return;
        }
        case swift::x86::X87Action::Free: {
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto physical = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto mask = context.GetTmpX();
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(physical.W(), fsw.W(), 11, 3);
            __ Add(physical.W(), physical.W(), index);
            __ And(physical.W(), physical.W(), 7);
            __ Lsl(shift.W(), physical.W(), 1);
            __ Mov(mask.W(), 3);
            __ Lsl(mask.W(), mask.W(), shift.W());
            __ Orr(ftw.W(), ftw.W(), mask.W());
            if (command_flags & swift::x86::X87Pop) {
                // Pop empties the old ST0 physical slot as well.
                __ Ubfx(physical.W(), fsw.W(), 11, 3);
                __ Lsl(shift.W(), physical.W(), 1);
                __ Mov(mask.W(), 3);
                __ Lsl(mask.W(), mask.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), mask.W());
                __ Add(physical.W(), physical.W(), 1);
                __ And(physical.W(), physical.W(), 7);
                __ And(fsw.W(), fsw.W(), 0xC5FF);
                __ Orr(fsw.W(), fsw.W(), Operand{physical.W(), LSL, 11});
            }
            __ Strh(ftw.W(), MemOperand(state, kFtw));
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            return;
        }
        case swift::x86::X87Action::Unary: {
            if (operation == static_cast<u8>(swift::x86::X87Unary::Sqrt)) {
                auto fsw = context.GetTmpX();
                auto ftw = context.GetTmpX();
                auto fcw = context.GetTmpX();
                auto physical = context.GetTmpX();
                auto shift = context.GetTmpX();
                auto bits = context.GetTmpX();
                auto sign_exp = context.GetTmpX();
                auto reg_address = context.GetTmpX();
                auto fp = context.GetTmpV();
                Label slow;
                Label no_pending;
                Label summary_done;
                Label done;
                __ Ldrh(fcw.W(), MemOperand(state, kFcw));
                __ And(shift.W(), fcw.W(), 0x0F00);
                __ Cmp(shift.W(), 0x0300);
                __ B(ne, &slow);
                __ Ldrh(fsw.W(), MemOperand(state, kFsw));
                __ Ldrh(ftw.W(), MemOperand(state, kFtw));
                __ Ubfx(physical.W(), fsw.W(), 11, 3);
                __ Lsl(shift.W(), physical.W(), 1);
                __ Lsr(bits.W(), ftw.W(), shift.W());
                __ And(bits.W(), bits.W(), 3);
                __ Cmp(bits.W(), 3);
                __ B(eq, &slow);
                __ Add(reg_address, state, kRegs);
                __ Add(reg_address,
                       reg_address,
                       Operand{physical, LSL, 4});
                __ Ldr(bits, MemOperand(reg_address));
                __ Ldrh(sign_exp.W(), MemOperand(reg_address, 8));
                __ Ldrb(shift.W(),
                        MemOperand(reg_address, kReducedMarkerOffset));
                __ Cmp(shift.W(), kReducedMarker);
                __ B(ne, &slow);
                __ Tbnz(sign_exp, 15, &slow);  // negative/NaN uses SoftFloat
                __ And(shift, bits, 0x7FF);
                __ Cbnz(shift, &slow);
                __ Tst(bits, 0x8000000000000000ull);
                __ B(eq, &slow);
                __ And(shift.W(), sign_exp.W(), 0x7FFF);
                __ Cmp(shift.W(), 0x3C01);
                __ B(lt, &slow);
                __ Cmp(shift.W(), 0x43FE);
                __ B(gt, &slow);
                __ Sub(sign_exp.W(), shift.W(), 0x3C00);
                __ Lsl(sign_exp, sign_exp, 52);
                __ Lsr(bits, bits, 11);
                __ And(bits, bits, 0x000FFFFFFFFFFFFFull);
                __ Orr(bits, bits, sign_exp);
                __ Fmov(fp.D(), bits);
                __ Msr(FPSR, xzr);
                __ Fsqrt(fp.D(), fp.D());
                __ Mrs(shift, FPSR);
                __ Fmov(bits, fp.D());

                // A positive normal f64 has a positive normal square root.
                __ Ubfx(sign_exp, bits, 52, 11);
                __ Add(sign_exp, sign_exp, 0x3C00);
                __ And(bits, bits, 0x000FFFFFFFFFFFFFull);
                __ Lsl(bits, bits, 11);
                __ Orr(bits, bits, 0x8000000000000000ull);
                __ Str(bits, MemOperand(reg_address));
                __ Strh(sign_exp.W(), MemOperand(reg_address, 8));
                __ Str(wzr, MemOperand(reg_address, 10));
                __ Strh(wzr, MemOperand(reg_address, 14));
                __ Mov(bits.W(), kReducedMarker);
                __ Strb(bits.W(),
                        MemOperand(reg_address, kReducedMarkerOffset));

                // FSQRT can only contribute precision for this guarded input
                // class (IXC bit 4 -> x87 PE bit 5).
                __ Ubfx(bits.W(), shift.W(), 4, 1);
                __ Orr(fsw.W(), fsw.W(), Operand{bits.W(), LSL, 5});
                __ And(fsw.W(), fsw.W(), 0xFDFF);
                __ And(bits.W(), fsw.W(), 0x3F);
                __ Bic(bits.W(), bits.W(), fcw.W());
                __ Cbz(bits.W(), &no_pending);
                __ Orr(fsw.W(), fsw.W(), 0x8080);
                __ B(&summary_done);
                __ Bind(&no_pending);
                __ And(fsw.W(), fsw.W(), 0x7F7F);
                __ Bind(&summary_done);
                __ Strh(fsw.W(), MemOperand(state, kFsw));
                zero_result();
                __ B(&done);
                __ Bind(&slow);
                fallback();
                __ Bind(&done);
                return;
            }
            if (operation != static_cast<u8>(swift::x86::X87Unary::ChangeSign) &&
                operation != static_cast<u8>(swift::x86::X87Unary::Abs)) {
                break;
            }
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto physical = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto reg_address = context.GetTmpX();
            Label slow;
            Label done;
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(physical.W(), fsw.W(), 11, 3);
            __ Lsl(scratch.W(), physical.W(), 1);
            __ Lsr(reg_address.W(), ftw.W(), scratch.W());
            __ And(reg_address.W(), reg_address.W(), 3);
            __ Cmp(reg_address.W(), 3);
            __ B(eq, &slow);
            __ Add(reg_address, state, kRegs);
            __ Add(reg_address,
                   reg_address,
                   Operand{physical, LSL, 4});
            __ Ldrh(scratch.W(), MemOperand(reg_address, 8));
            if (operation == static_cast<u8>(swift::x86::X87Unary::ChangeSign)) {
                __ Eor(scratch.W(), scratch.W(), 0x8000);
            } else {
                __ And(scratch.W(), scratch.W(), 0x7FFF);
            }
            __ Strh(scratch.W(), MemOperand(reg_address, 8));
            __ And(fsw.W(), fsw.W(), 0xFDFF);
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();
            __ Bind(&done);
            return;
        }
        case swift::x86::X87Action::Exchange: {
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto physical0 = context.GetTmpX();
            auto physical1 = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto address0 = context.GetTmpX();
            auto value0 = context.GetTmpV();
            auto value1 = context.GetTmpV();
            Label slow;
            Label done;
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(physical0.W(), fsw.W(), 11, 3);
            __ Add(physical1.W(), physical0.W(), index);
            __ And(physical1.W(), physical1.W(), 7);
            for (auto physical : {physical0, physical1}) {
                __ Lsl(scratch.W(), physical.W(), 1);
                __ Lsr(address0.W(), ftw.W(), scratch.W());
                __ And(address0.W(), address0.W(), 3);
                __ Cmp(address0.W(), 3);
                __ B(eq, &slow);
            }
            __ Add(address0, state, kRegs);
            __ Add(address0, address0, Operand{physical0, LSL, 4});
            __ Add(scratch, state, kRegs);
            __ Add(scratch, scratch, Operand{physical1, LSL, 4});
            __ Ldr(value0.Q(), MemOperand(address0));
            __ Ldr(value1.Q(), MemOperand(scratch));
            __ Str(value1.Q(), MemOperand(address0));
            __ Str(value0.Q(), MemOperand(scratch));
            __ And(fsw.W(), fsw.W(), 0xFDFF);
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();
            __ Bind(&done);
            return;
        }
        case swift::x86::X87Action::LoadReg: {
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto source = context.GetTmpX();
            auto dest = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto value = context.GetTmpV();
            Label slow;
            Label done;
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(dest.W(), fsw.W(), 11, 3);
            __ Add(source.W(), dest.W(), index);
            __ And(source.W(), source.W(), 7);
            __ Lsl(shift.W(), source.W(), 1);
            __ Lsr(scratch.W(), ftw.W(), shift.W());
            __ And(scratch.W(), scratch.W(), 3);  // source tag
            __ Cmp(scratch.W(), 3);
            __ B(eq, &slow);
            __ Add(dest.W(), dest.W(), 7);
            __ And(dest.W(), dest.W(), 7);
            __ Lsl(shift.W(), dest.W(), 1);
            auto dest_tag = context.GetTmpX();
            __ Lsr(dest_tag.W(), ftw.W(), shift.W());
            __ And(dest_tag.W(), dest_tag.W(), 3);
            __ Cmp(dest_tag.W(), 3);
            __ B(ne, &slow);
            auto source_address = context.GetTmpX();
            __ Add(source_address, state, kRegs);
            __ Add(source_address,
                   source_address,
                   Operand{source, LSL, 4});
            __ Add(dest_tag, state, kRegs);
            __ Add(dest_tag, dest_tag, Operand{dest, LSL, 4});
            __ Ldr(value.Q(), MemOperand(source_address));
            __ Str(value.Q(), MemOperand(dest_tag));
            __ Mov(dest_tag.W(), 3);
            __ Lsl(dest_tag.W(), dest_tag.W(), shift.W());
            __ Bic(ftw.W(), ftw.W(), dest_tag.W());
            __ Lsl(scratch.W(), scratch.W(), shift.W());
            __ Orr(ftw.W(), ftw.W(), scratch.W());
            __ And(fsw.W(), fsw.W(), 0xC5FF);
            __ Orr(fsw.W(), fsw.W(), Operand{dest.W(), LSL, 11});
            __ Strh(ftw.W(), MemOperand(state, kFtw));
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();
            __ Bind(&done);
            return;
        }
        case swift::x86::X87Action::StoreReg: {
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto source = context.GetTmpX();
            auto dest = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto tag = context.GetTmpX();
            auto value = context.GetTmpV();
            Label slow;
            Label done;
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(source.W(), fsw.W(), 11, 3);
            __ Lsl(shift.W(), source.W(), 1);
            __ Lsr(tag.W(), ftw.W(), shift.W());
            __ And(tag.W(), tag.W(), 3);
            __ Cmp(tag.W(), 3);
            __ B(eq, &slow);
            __ Add(dest.W(), source.W(), index);
            __ And(dest.W(), dest.W(), 7);
            auto source_address = context.GetTmpX();
            auto dest_address = context.GetTmpX();
            __ Add(source_address, state, kRegs);
            __ Add(source_address,
                   source_address,
                   Operand{source, LSL, 4});
            __ Add(dest_address, state, kRegs);
            __ Add(dest_address,
                   dest_address,
                   Operand{dest, LSL, 4});
            __ Ldr(value.Q(), MemOperand(source_address));
            __ Str(value.Q(), MemOperand(dest_address));
            __ Lsl(shift.W(), dest.W(), 1);
            __ Mov(dest_address.W(), 3);
            __ Lsl(dest_address.W(), dest_address.W(), shift.W());
            __ Bic(ftw.W(), ftw.W(), dest_address.W());
            __ Lsl(tag.W(), tag.W(), shift.W());
            __ Orr(ftw.W(), ftw.W(), tag.W());
            if (command_flags & swift::x86::X87Pop) {
                __ Lsl(shift.W(), source.W(), 1);
                __ Mov(dest_address.W(), 3);
                __ Lsl(dest_address.W(), dest_address.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), dest_address.W());
                __ Add(source.W(), source.W(), 1);
                __ And(source.W(), source.W(), 7);
                __ And(fsw.W(), fsw.W(), 0xC5FF);
                __ Orr(fsw.W(), fsw.W(), Operand{source.W(), LSL, 11});
            } else {
                __ And(fsw.W(), fsw.W(), 0xFDFF);
            }
            __ Strh(ftw.W(), MemOperand(state, kFtw));
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();
            __ Bind(&done);
            return;
        }
        case swift::x86::X87Action::LoadConstant: {
            static constexpr std::array<u64, 7> kSignificands{
                    0x8000000000000000ull,
                    0xD49A784BCD1B8AFEull,
                    0xB8AA3B295C17F0BCull,
                    0xC90FDAA22168C235ull,
                    0x9A209A84FBCFF799ull,
                    0xB17217F7D1CF79ACull,
                    0,
            };
            static constexpr std::array<u16, 7> kSignExponents{
                    0x3FFF, 0x4000, 0x3FFF, 0x4000, 0x3FFD, 0x3FFE, 0,
            };
            ASSERT(operation < kSignificands.size());
            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto top = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto reg_address = context.GetTmpX();
            Label slow;
            Label done;
            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(top.W(), fsw.W(), 11, 3);
            __ Add(top.W(), top.W(), 7);
            __ And(top.W(), top.W(), 7);
            __ Lsl(shift.W(), top.W(), 1);
            __ Lsr(scratch.W(), ftw.W(), shift.W());
            __ And(scratch.W(), scratch.W(), 3);
            __ Cmp(scratch.W(), 3);
            __ B(ne, &slow);
            __ Add(reg_address, state, kRegs);
            __ Add(reg_address,
                   reg_address,
                   Operand{top, LSL, 4});
            __ Mov(scratch, kSignificands[operation]);
            __ Mov(shift, kSignExponents[operation]);
            __ Stp(scratch, shift, MemOperand(reg_address));
            __ Mov(scratch.W(), kReducedMarker);
            __ Strb(scratch.W(),
                    MemOperand(reg_address, kReducedMarkerOffset));
            __ Mov(reg_address.W(), 3);
            __ Lsl(reg_address.W(), reg_address.W(), top.W());
            __ Lsl(reg_address.W(), reg_address.W(), top.W());
            __ Bic(ftw.W(), ftw.W(), reg_address.W());
            if (operation == static_cast<u8>(swift::x86::X87Constant::Zero)) {
                __ Mov(reg_address.W(), 1);
                __ Lsl(shift.W(), top.W(), 1);
                __ Lsl(reg_address.W(), reg_address.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), reg_address.W());
            }
            __ And(fsw.W(), fsw.W(), 0xC5FF);
            __ Orr(fsw.W(), fsw.W(), Operand{top.W(), LSL, 11});
            __ Strh(ftw.W(), MemOperand(state, kFtw));
            __ Strh(fsw.W(), MemOperand(state, kFsw));
            zero_result();
            __ B(&done);
            __ Bind(&slow);
            fallback();
            __ Bind(&done);
            return;
        }
        default:
            break;
    }

    // Arithmetic, conversions, comparisons, ext80 memory, and all
    // transcendental/remainder operations remain on the exact SoftFloat
    // dispatch until their reduced-precision fast paths have passed the
    // dedicated differential suite.
    (void)operation;
    fallback();
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
