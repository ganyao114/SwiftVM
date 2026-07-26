#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/frontend/x86/x87.h"
#include "translator/x86/cpu.h"

#define __ masm.


namespace swift::runtime::backend::arm64 {

void JitTranslator::EmitX87Op(ir::Inst* inst) {
    // VIXL MacroAssembler may clobber ip0/ip1 while materializing immediates.
    // Keep them out of this instruction's long-lived temporary set without
    // shrinking the register allocator globally (which can force unrelated,
    // high-pressure emitters into the spill path).
    context.ReserveTmpX(ip0);
    context.ReserveTmpX(ip1);

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
        case swift::x86::X87Action::LoadInt: {
            if (format != swift::x86::X87Format::Int16 &&
                format != swift::x86::X87Format::Int32 &&
                format != swift::x86::X87Format::Int64) {
                break;
            }

            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto top = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto integer = context.GetTmpX();
            auto bits = context.GetTmpX();
            auto reg_address = context.GetTmpX();
            auto guest = context.X(address);
            auto fp = context.GetTmpV();
            Label slow;
            Label zero;
            Label encoded;
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
            __ B(ne, &slow);  // occupied push destination: exact stack fault

            const auto mem = use_memory_base ? BiasMem(guest) : MemOperand(guest);
            if (format == swift::x86::X87Format::Int16) {
                __ Ldrsh(integer, mem);
            } else if (format == swift::x86::X87Format::Int32) {
                __ Ldrsw(integer, mem);
            } else {
                __ Ldr(integer, mem);
                // Only integers with |x| <= 2^53 are certified: their f64
                // conversion and expansion are bit-identical to SoftFloat
                // ext80. Larger m64 integers bail instead of introducing a
                // new reduced-precision architectural value.
                __ Cmp(integer, 0);
                __ Cneg(scratch, integer, mi);
                __ Mov(bits, UINT64_C(0x0020000000000000));
                __ Cmp(scratch, bits);
                __ B(hi, &slow);
            }

            __ Scvtf(fp.D(), integer);
            __ Fmov(bits, fp.D());
            __ Cbz(integer, &zero);
            __ Ubfx(scratch, bits, 52, 11);
            __ Add(scratch, scratch, 0x3C00);
            __ Ubfx(reg_address, bits, 63, 1);
            __ Orr(scratch, scratch, Operand{reg_address, LSL, 15});
            __ And(bits, bits, UINT64_C(0x000FFFFFFFFFFFFF));
            __ Lsl(bits, bits, 11);
            __ Orr(bits, bits, UINT64_C(0x8000000000000000));
            __ B(&encoded);
            __ Bind(&zero);
            __ Mov(bits, 0);
            __ Mov(scratch, 0);
            __ Bind(&encoded);

            __ Add(reg_address, state, kRegs);
            __ Add(reg_address, reg_address, Operand{top, LSL, 4});
            __ Str(bits, MemOperand(reg_address));
            __ Strh(scratch.W(), MemOperand(reg_address, 8));
            __ Str(wzr, MemOperand(reg_address, 10));
            __ Strh(wzr, MemOperand(reg_address, 14));
            __ Mov(scratch.W(), kReducedMarker);
            __ Strb(scratch.W(),
                    MemOperand(reg_address, kReducedMarkerOffset));

            __ Mov(scratch.W(), 3);
            __ Lsl(scratch.W(), scratch.W(), shift.W());
            __ Bic(ftw.W(), ftw.W(), scratch.W());
            __ Cmp(integer, 0);
            __ Cset(scratch.W(), eq);  // zero tag=1, valid tag=0
            __ Lsl(scratch.W(), scratch.W(), shift.W());
            __ Orr(ftw.W(), ftw.W(), scratch.W());
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
        case swift::x86::X87Action::StoreInt: {
            if (format != swift::x86::X87Format::Int16 &&
                format != swift::x86::X87Format::Int32 &&
                format != swift::x86::X87Format::Int64) {
                break;
            }

            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto fcw = context.GetTmpX();
            auto top = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto bits = context.GetTmpX();
            auto sign_exp = context.GetTmpX();
            auto reg_address = context.GetTmpX();
            auto scratch = atomic_scratch;
            auto converted = atomic_pair_scratch;
            auto guest = context.X(address);
            auto input_fp = context.GetTmpV();
            auto rounded_fp = context.GetTmpV();
            Label slow;
            Label input_zero;
            Label input_ready;
            Label round_nearest;
            Label round_down;
            Label round_up;
            Label round_zero;
            Label rounded;
            Label invalid;
            Label store;
            Label no_pending;
            Label summary_done;
            Label done;

            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ldrh(fcw.W(), MemOperand(state, kFcw));
            __ Ubfx(top.W(), fsw.W(), 11, 3);
            __ Lsl(shift.W(), top.W(), 1);
            __ Lsr(sign_exp.W(), ftw.W(), shift.W());
            __ And(sign_exp.W(), sign_exp.W(), 3);
            __ Cmp(sign_exp.W(), 3);
            __ B(eq, &slow);  // empty stack uses exact stack-fault helper

            __ Add(reg_address, state, kRegs);
            __ Add(reg_address, reg_address, Operand{top, LSL, 4});
            __ Ldrb(scratch.W(),
                    MemOperand(reg_address, kReducedMarkerOffset));
            __ Cmp(scratch.W(), kReducedMarker);
            // A6/unmarked ext80 values may contain precision below binary64;
            // those conversions must remain on SoftFloat.
            __ B(ne, &slow);
            __ Ldr(bits, MemOperand(reg_address));
            __ Ldrh(sign_exp.W(), MemOperand(reg_address, 8));
            __ And(shift.W(), sign_exp.W(), 0x7FFF);
            __ Cbz(shift.W(), &input_zero);
            __ Cmp(shift.W(), 0x7FFF);
            __ B(eq, &invalid);  // every NaN and infinity is integer-invalid
            __ Cmp(shift.W(), 0x43FE);
            __ B(gt, &invalid);  // certainly outside every integer width
            __ Cmp(shift.W(), 0x3C01);
            __ B(lt, &slow);     // f64-subnormal/ext80-only inputs use helper
            __ Tst(bits, UINT64_C(0x8000000000000000));
            __ B(eq, &slow);
            __ And(scratch, bits, 0x7FF);
            __ Cbnz(scratch, &slow);
            __ Lsr(bits, bits, 11);
            __ Sub(shift.W(), shift.W(), 0x3C00);
            __ Lsl(shift, shift, 52);
            __ And(bits, bits, UINT64_C(0x000FFFFFFFFFFFFF));
            __ Orr(bits, bits, shift);
            __ Ubfx(sign_exp, sign_exp, 15, 1);
            __ Orr(bits, bits, Operand{sign_exp, LSL, 63});
            __ B(&input_ready);
            __ Bind(&input_zero);
            __ Cbnz(bits, &slow);  // true ext80 denormal: preserve DE/helper
            __ Ubfx(bits, sign_exp, 15, 1);
            __ Lsl(bits, bits, 63);
            __ Bind(&input_ready);
            __ Fmov(input_fp.D(), bits);

            if (command_flags & swift::x86::X87Truncate) {
                __ B(&round_zero);
            } else {
                __ Ubfx(scratch.W(), fcw.W(), 10, 2);
                __ Cbz(scratch.W(), &round_nearest);
                __ Cmp(scratch.W(), 1);
                __ B(eq, &round_down);
                __ Cmp(scratch.W(), 2);
                __ B(eq, &round_up);
                __ B(&round_zero);
            }
            __ Bind(&round_nearest);
            __ Frintn(rounded_fp.D(), input_fp.D());
            __ B(&rounded);
            __ Bind(&round_down);
            __ Frintm(rounded_fp.D(), input_fp.D());
            __ B(&rounded);
            __ Bind(&round_up);
            __ Frintp(rounded_fp.D(), input_fp.D());
            __ B(&rounded);
            __ Bind(&round_zero);
            __ Frintz(rounded_fp.D(), input_fp.D());
            __ Bind(&rounded);

            const u64 upper_bits =
                    format == swift::x86::X87Format::Int16
                            ? UINT64_C(0x40E0000000000000)
                    : format == swift::x86::X87Format::Int32
                            ? UINT64_C(0x41E0000000000000)
                            : UINT64_C(0x43E0000000000000);
            const u64 lower_bits =
                    format == swift::x86::X87Format::Int16
                            ? UINT64_C(0xC0E0000000000000)
                    : format == swift::x86::X87Format::Int32
                            ? UINT64_C(0xC1E0000000000000)
                            : UINT64_C(0xC3E0000000000000);
            __ Mov(scratch, upper_bits);
            __ Fmov(input_fp.D(), scratch);
            __ Fcmp(rounded_fp.D(), input_fp.D());
            __ B(ge, &invalid);  // +2^(N-1) is the first invalid value
            __ Mov(scratch, lower_bits);
            __ Fmov(input_fp.D(), scratch);
            __ Fcmp(rounded_fp.D(), input_fp.D());
            __ B(lt, &invalid);  // -2^(N-1) itself remains valid

            if (format == swift::x86::X87Format::Int64) {
                __ Fcvtzs(converted, rounded_fp.D());
            } else {
                __ Fcvtzs(converted.W(), rounded_fp.D());
            }
            // Match SoftFloat's exact=true conversion: valid non-integral
            // inputs contribute PE, while invalid conversions contribute IE
            // only. FRINTN/M/P/Z themselves are deliberately used instead of
            // changing process-global FPCR.
            __ Fmov(input_fp.D(), bits);
            __ Fcmp(input_fp.D(), rounded_fp.D());
            __ Cset(scratch.W(), ne);
            __ Orr(fsw.W(), fsw.W(), Operand{scratch.W(), LSL, 5});
            __ B(&store);

            __ Bind(&invalid);
            __ Mov(converted,
                   format == swift::x86::X87Format::Int16
                           ? UINT64_C(0x8000)
                   : format == swift::x86::X87Format::Int32
                           ? UINT64_C(0x80000000)
                           : UINT64_C(0x8000000000000000));
            __ Orr(fsw.W(), fsw.W(), 1);  // IE

            __ Bind(&store);
            __ And(fsw.W(), fsw.W(), 0xFDFF);  // C1=0
            const auto out = use_memory_base ? BiasMem(guest) : MemOperand(guest);
            if (format == swift::x86::X87Format::Int16) {
                __ Strh(converted.W(), out);
            } else if (format == swift::x86::X87Format::Int32) {
                __ Str(converted.W(), out);
            } else {
                __ Str(converted, out);
            }

            if (command_flags & swift::x86::X87Pop) {
                __ Lsl(shift.W(), top.W(), 1);
                __ Mov(scratch.W(), 3);
                __ Lsl(scratch.W(), scratch.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), scratch.W());
                __ Add(top.W(), top.W(), 1);
                __ And(top.W(), top.W(), 7);
                __ And(fsw.W(), fsw.W(), 0xC5FF);
                __ Orr(fsw.W(), fsw.W(), Operand{top.W(), LSL, 11});
                __ Strh(ftw.W(), MemOperand(state, kFtw));
            }

            __ And(scratch.W(), fsw.W(), 0x3F);
            __ Bic(scratch.W(), scratch.W(), fcw.W());
            __ Cbz(scratch.W(), &no_pending);
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
        case swift::x86::X87Action::Binary: {
            // Reduced-precision register and m32real/m64real arithmetic. The
            // architectural state remains ext80; stack operands must carry
            // certified f64 provenance. Memory NaN/Inf values, empty/special
            // stack values, non-RNE control words, and PC below 64 all bail to
            // the bit-exact SoftFloat implementation. Every finite normal f32
            // is exactly representable as f64; m64real is already binary64.
            const bool register_form =
                    format == swift::x86::X87Format::Register;
            const bool memory_real =
                    format == swift::x86::X87Format::Float32 ||
                    format == swift::x86::X87Format::Float64;
            if (!register_form && !memory_real) {
                break;
            }

            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto fcw = context.GetTmpX();
            auto left_physical = context.GetTmpX();
            auto right_physical = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto left_address = context.GetTmpX();
            // x12/x13 are permanently reserved by the trampoline mask for
            // atomic emitters. X87Op is mutually exclusive with an atomic
            // instruction, so using them explicitly bounds this emitter's
            // generic scratch demand without risking a live guest value.
            auto right_address = atomic_pair_scratch;
            auto left_bits = context.GetTmpX();
            auto right_bits = atomic_scratch;
            // FCW is dead after the entry guard and is overwritten with FPSR
            // before its next use, so it can carry the conversion marker.
            auto round_bits = fcw;
            auto left_fp = context.GetTmpV();
            auto right_fp = context.GetTmpV();
            Register guest{};
            if (memory_real) {
                guest = context.X(address);
            }
            Label slow;
            Label done;

            __ Ldrh(fcw.W(), MemOperand(state, kFcw));
            __ And(scratch.W(), fcw.W(), 0x0F00);
            __ Cmp(scratch.W(), 0x0300);  // PC=64 and RC=nearest-even.
            __ B(ne, &slow);

            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(left_physical.W(), fsw.W(), 11, 3);
            if (register_form &&
                (command_flags & swift::x86::X87DestIndex)) {
                __ Add(left_physical.W(), left_physical.W(), index);
                __ And(left_physical.W(), left_physical.W(), 7);
                __ Ubfx(right_physical.W(), fsw.W(), 11, 3);
            } else if (register_form) {
                __ Add(right_physical.W(), left_physical.W(), index);
                __ And(right_physical.W(), right_physical.W(), 7);
            }

            // Stack source tags must be non-empty. Special values deliberately
            // stay on SoftFloat even when their payload happens to fit f64.
            auto require_nonempty = [&](const Register& physical) {
                __ Lsl(scratch.W(), physical.W(), 1);
                __ Lsr(scratch.W(), ftw.W(), scratch.W());
                __ And(scratch.W(), scratch.W(), 3);
                __ Cmp(scratch.W(), 3);
                __ B(eq, &slow);
            };
            require_nonempty(left_physical);
            if (register_form) {
                require_nonempty(right_physical);
            }

            __ Add(left_address, state, kRegs);
            __ Add(left_address,
                   left_address,
                   Operand{left_physical, LSL, 4});
            if (register_form) {
                __ Add(right_address, state, kRegs);
                __ Add(right_address,
                       right_address,
                       Operand{right_physical, LSL, 4});
            }

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
            if (register_form) {
                convert_one(right_address, right_bits, left_bits, right_fp);
            } else if (format == swift::x86::X87Format::Float32) {
                __ Ldr(right_bits.W(),
                       use_memory_base ? BiasMem(guest) : MemOperand(guest));
                // ARM and x87 differ in NaN propagation/invalid-result details;
                // keep all memory NaN/Inf and denormal inputs on SoftFloat.
                __ And(left_bits.W(), right_bits.W(), 0x7F800000u);
                __ Cmp(left_bits.W(), 0x7F800000u);
                __ B(eq, &slow);
                Label memory_f32_ready;
                __ Cbnz(left_bits.W(), &memory_f32_ready);
                __ And(left_bits.W(), right_bits.W(), 0x007FFFFFu);
                __ Cbnz(left_bits.W(), &slow);  // preserve x87 DE
                __ Bind(&memory_f32_ready);
                __ Fmov(right_fp.S(), right_bits.W());
                __ Fcvt(right_fp.D(), right_fp.S());
            } else {
                __ Ldr(right_bits,
                       use_memory_base ? BiasMem(guest) : MemOperand(guest));
                __ Ubfx(left_bits, right_bits, 52, 11);
                __ Cmp(left_bits, 0x7FF);
                __ B(eq, &slow);
                Label memory_f64_ready;
                __ Cbnz(left_bits, &memory_f64_ready);
                __ And(left_bits,
                       right_bits,
                       UINT64_C(0x000FFFFFFFFFFFFF));
                __ Cbnz(left_bits, &slow);  // preserve x87 DE
                __ Bind(&memory_f64_ready);
                __ Fmov(right_fp.D(), right_bits);
            }

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
            // ARM's invalid-operation default NaN encoding/propagation is not
            // the x87 SoftFloat result (notably 0/0). Keep every IOC result on
            // the exact helper before publishing architectural state.
            __ Tbnz(fcw, 0, &slow);  // FPSR.IOC

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
        case swift::x86::X87Action::Compare: {
            const bool register_form =
                    format == swift::x86::X87Format::Register;
            const bool memory_real =
                    format == swift::x86::X87Format::Float32 ||
                    format == swift::x86::X87Format::Float64;
            if (!register_form && !memory_real) {
                break;  // integer-memory compares stay on SoftFloat
            }

            auto fsw = context.GetTmpX();
            auto ftw = context.GetTmpX();
            auto left_physical = context.GetTmpX();
            auto right_physical = context.GetTmpX();
            auto shift = context.GetTmpX();
            auto scratch = context.GetTmpX();
            auto left_address = context.GetTmpX();
            auto left_bits = context.GetTmpX();
            auto right_address = atomic_pair_scratch;
            auto right_bits = atomic_scratch;
            auto left_fp = context.GetTmpV();
            auto right_fp = context.GetTmpV();
            Register guest{};
            if (memory_real) {
                guest = context.X(address);
            }
            Label slow;
            Label done;

            __ Ldrh(fsw.W(), MemOperand(state, kFsw));
            __ Ldrh(ftw.W(), MemOperand(state, kFtw));
            __ Ubfx(left_physical.W(), fsw.W(), 11, 3);
            __ Lsl(shift.W(), left_physical.W(), 1);
            __ Lsr(scratch.W(), ftw.W(), shift.W());
            __ And(scratch.W(), scratch.W(), 3);
            __ Cmp(scratch.W(), 3);
            __ B(eq, &slow);
            if (register_form) {
                __ Add(right_physical.W(), left_physical.W(), index);
                __ And(right_physical.W(), right_physical.W(), 7);
                __ Lsl(shift.W(), right_physical.W(), 1);
                __ Lsr(scratch.W(), ftw.W(), shift.W());
                __ And(scratch.W(), scratch.W(), 3);
                __ Cmp(scratch.W(), 3);
                __ B(eq, &slow);
            }

            __ Add(left_address, state, kRegs);
            __ Add(left_address,
                   left_address,
                   Operand{left_physical, LSL, 4});
            if (register_form) {
                __ Add(right_address, state, kRegs);
                __ Add(right_address,
                       right_address,
                       Operand{right_physical, LSL, 4});
            }

            auto convert_canonical = [&](const Register& ext_address,
                                         const Register& bits,
                                         const Register& exponent,
                                         const VRegister& fp) {
                auto sign_exp = scratch;
                Label input_zero;
                Label converted;
                __ Ldrb(shift.W(),
                        MemOperand(ext_address, kReducedMarkerOffset));
                __ Cmp(shift.W(), kReducedMarker);
                // A6 can carry ext80-only tail bits. NaN, infinity, ext80
                // denormals, and every uncertified value bail so FCOM/FUCOM
                // exception distinctions remain the helper's responsibility.
                __ B(ne, &slow);
                __ Ldr(bits, MemOperand(ext_address));
                __ Ldrh(sign_exp.W(), MemOperand(ext_address, 8));
                __ And(exponent.W(), sign_exp.W(), 0x7FFF);
                __ Cbz(exponent.W(), &input_zero);
                __ Cmp(exponent.W(), 0x3C01);
                __ B(lt, &slow);
                __ Cmp(exponent.W(), 0x43FE);
                __ B(gt, &slow);
                __ Tst(bits, UINT64_C(0x8000000000000000));
                __ B(eq, &slow);
                __ And(shift, bits, 0x7FF);
                __ Cbnz(shift, &slow);
                __ Lsr(bits, bits, 11);
                __ Sub(exponent.W(), exponent.W(), 0x3C00);
                __ Lsl(exponent, exponent, 52);
                __ And(bits, bits, UINT64_C(0x000FFFFFFFFFFFFF));
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

            convert_canonical(left_address, left_bits, right_bits, left_fp);
            if (register_form) {
                convert_canonical(right_address,
                                  right_bits,
                                  left_bits,
                                  right_fp);
            } else if (format == swift::x86::X87Format::Float32) {
                __ Ldr(right_bits.W(),
                       use_memory_base ? BiasMem(guest) : MemOperand(guest));
                __ And(left_bits.W(), right_bits.W(), 0x7F800000u);
                __ Cmp(left_bits.W(), 0x7F800000u);
                __ B(eq, &slow);  // memory NaN/Inf: helper owns IE semantics
                Label memory_f32_ready;
                __ Cbnz(left_bits.W(), &memory_f32_ready);
                __ And(left_bits.W(), right_bits.W(), 0x007FFFFFu);
                __ Cbnz(left_bits.W(),
                         &slow);  // preserve helper status behavior
                __ Bind(&memory_f32_ready);
                __ Fmov(right_fp.S(), right_bits.W());
                __ Fcvt(right_fp.D(), right_fp.S());
            } else {
                __ Ldr(right_bits,
                       use_memory_base ? BiasMem(guest) : MemOperand(guest));
                __ Ubfx(left_bits, right_bits, 52, 11);
                __ Cmp(left_bits, 0x7FF);
                __ B(eq, &slow);  // memory NaN/Inf: helper owns IE semantics
                Label memory_f64_ready;
                __ Cbnz(left_bits, &memory_f64_ready);
                __ And(left_bits,
                       right_bits,
                       UINT64_C(0x000FFFFFFFFFFFFF));
                __ Cbnz(left_bits,
                         &slow);  // preserve helper status behavior
                __ Bind(&memory_f64_ready);
                __ Fmov(right_fp.D(), right_bits);
            }

            __ Fcmp(left_fp.D(), right_fp.D());
            __ Cset(scratch.W(), lt);
            if (command_flags & swift::x86::X87ToEFlags) {
                ASSERT(has_result);
                __ Mov(result.W(), scratch.W());  // CF
                __ Cset(shift.W(), eq);
                __ Orr(result.W(),
                       result.W(),
                       Operand{shift.W(), LSL, 2});  // ZF; PF remains zero
            } else {
                __ And(fsw.W(), fsw.W(), 0xB8FF);  // clear C3/C2/C1/C0
                __ Orr(fsw.W(),
                       fsw.W(),
                       Operand{scratch.W(), LSL, 8});  // less -> C0
                __ Cset(shift.W(), eq);
                __ Orr(fsw.W(),
                       fsw.W(),
                       Operand{shift.W(), LSL, 14});  // equal -> C3
                zero_result();
            }

            if (command_flags &
                (swift::x86::X87Pop | swift::x86::X87PopTwice)) {
                __ Lsl(shift.W(), left_physical.W(), 1);
                __ Mov(scratch.W(), 3);
                __ Lsl(scratch.W(), scratch.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), scratch.W());
                __ Add(left_physical.W(), left_physical.W(), 1);
                __ And(left_physical.W(), left_physical.W(), 7);
                if (command_flags & swift::x86::X87PopTwice) {
                    __ Lsl(shift.W(), left_physical.W(), 1);
                    __ Mov(scratch.W(), 3);
                    __ Lsl(scratch.W(), scratch.W(), shift.W());
                    __ Orr(ftw.W(), ftw.W(), scratch.W());
                    __ Add(left_physical.W(), left_physical.W(), 1);
                    __ And(left_physical.W(), left_physical.W(), 7);
                }
                __ And(fsw.W(), fsw.W(), 0xC5FF);
                __ Orr(fsw.W(),
                       fsw.W(),
                       Operand{left_physical.W(), LSL, 11});
                __ Strh(ftw.W(), MemOperand(state, kFtw));
                __ Strh(fsw.W(), MemOperand(state, kFsw));
            } else if (!(command_flags & swift::x86::X87ToEFlags)) {
                __ Strh(fsw.W(), MemOperand(state, kFsw));
            }
            __ B(&done);
            __ Bind(&slow);
            fallback();
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
            if (operation == static_cast<u8>(swift::x86::X87Unary::Test)) {
                auto fsw = context.GetTmpX();
                auto ftw = context.GetTmpX();
                auto physical = context.GetTmpX();
                auto shift = context.GetTmpX();
                auto bits = context.GetTmpX();
                auto sign_exp = context.GetTmpX();
                auto reg_address = context.GetTmpX();
                auto fp = context.GetTmpV();
                auto zero_fp = context.GetTmpV();
                Label slow;
                Label input_zero;
                Label converted;
                Label done;

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
                __ Ldrb(shift.W(),
                        MemOperand(reg_address, kReducedMarkerOffset));
                __ Cmp(shift.W(), kReducedMarker);
                // NaN, ext80 denormal, and uncertified values bail so FTST
                // retains the helper's IE/DE and unordered behavior.
                __ B(ne, &slow);
                __ Ldr(bits, MemOperand(reg_address));
                __ Ldrh(sign_exp.W(), MemOperand(reg_address, 8));
                __ And(shift.W(), sign_exp.W(), 0x7FFF);
                __ Cbz(shift.W(), &input_zero);
                __ Cmp(shift.W(), 0x3C01);
                __ B(lt, &slow);
                __ Cmp(shift.W(), 0x43FE);
                __ B(gt, &slow);
                __ Tst(bits, UINT64_C(0x8000000000000000));
                __ B(eq, &slow);
                __ And(physical, bits, 0x7FF);
                __ Cbnz(physical, &slow);
                __ Lsr(bits, bits, 11);
                __ Sub(shift.W(), shift.W(), 0x3C00);
                __ Lsl(shift, shift, 52);
                __ And(bits, bits, UINT64_C(0x000FFFFFFFFFFFFF));
                __ Orr(bits, bits, shift);
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
                __ Fmov(zero_fp.D(), xzr);
                __ Fcmp(fp.D(), zero_fp.D());
                __ And(fsw.W(), fsw.W(), 0xB8FF);
                __ Cset(bits.W(), lt);
                __ Orr(fsw.W(), fsw.W(), Operand{bits.W(), LSL, 8});
                __ Cset(bits.W(), eq);
                __ Orr(fsw.W(), fsw.W(), Operand{bits.W(), LSL, 14});
                __ Strh(fsw.W(), MemOperand(state, kFsw));
                zero_result();
                __ B(&done);
                __ Bind(&slow);
                fallback();
                __ Bind(&done);
                return;
            }
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
                Label input_zero;
                Label input_special;
                Label positive_infinity;
                Label quiet_nan;
                Label invalid;
                Label publish_special;
                Label status;
                Label no_pending;
                Label summary_done;
                Label done;
                __ Ldrh(fcw.W(), MemOperand(state, kFcw));
                __ And(shift.W(), fcw.W(), 0x0F00);
                __ Cmp(shift.W(), 0x0300);
                // Host FSQRT is RNE/binary64. Other RC/PC modes and ext80
                // denormals stay on SoftFloat.
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
                __ And(shift.W(), sign_exp.W(), 0x7FFF);
                __ Cbz(shift.W(), &input_zero);
                __ Cmp(shift.W(), 0x7FFF);
                __ B(eq, &input_special);
                __ Tbnz(sign_exp, 15, &invalid);
                __ And(shift, bits, 0x7FF);
                __ Cbnz(shift, &slow);
                __ Tst(bits, UINT64_C(0x8000000000000000));
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
                // An inexact binary64 square root may retain additional low
                // significand bits in ext80. Publish only exact host results;
                // all irrational/double-rounding cases stay on SoftFloat.
                __ Tbnz(shift, 4, &slow);  // FPSR.IXC
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

                // IXC was rejected before publishing, so this accepted
                // positive-finite result is bit-identical to ext80 SoftFloat.
                __ B(&status);

                __ Bind(&input_zero);
                __ Cbnz(bits, &slow);  // true ext80 denormal -> helper + DE
                // sqrt(+/-0) preserves the input image and provenance.
                __ B(&status);

                __ Bind(&input_special);
                __ Mov(shift, UINT64_C(0x8000000000000000));
                __ Cmp(bits, shift);
                __ B(eq, &positive_infinity);
                // QNaN propagates its payload; SNaN is quieted and raises IE.
                __ Tbnz(bits, 62, &quiet_nan);
                __ Orr(fsw.W(), fsw.W(), 1);
                __ Bind(&quiet_nan);
                __ Orr(bits, bits, UINT64_C(0xC000000000000000));
                __ B(&publish_special);

                __ Bind(&positive_infinity);
                __ Tbnz(sign_exp, 15, &invalid);
                __ B(&publish_special);

                __ Bind(&invalid);
                // Negative finite values and -Inf produce the x87 indefinite
                // QNaN, not ARM's positive default NaN.
                __ Mov(bits, UINT64_C(0xC000000000000000));
                __ Mov(sign_exp.W(), 0xFFFF);
                __ Orr(fsw.W(), fsw.W(), 1);

                __ Bind(&publish_special);
                __ Str(bits, MemOperand(reg_address));
                __ Strh(sign_exp.W(), MemOperand(reg_address, 8));
                __ Str(wzr, MemOperand(reg_address, 10));
                __ Strh(wzr, MemOperand(reg_address, 14));
                __ Mov(shift.W(), kReducedMarker);
                __ Strb(shift.W(),
                        MemOperand(reg_address, kReducedMarkerOffset));
                // Reclassify ST0 as special (covers NaN, infinity, and the
                // negative-input indefinite result).
                __ Lsl(shift.W(), physical.W(), 1);
                __ Mov(bits.W(), 3);
                __ Lsl(bits.W(), bits.W(), shift.W());
                __ Bic(ftw.W(), ftw.W(), bits.W());
                __ Mov(bits.W(), 2);
                __ Lsl(bits.W(), bits.W(), shift.W());
                __ Orr(ftw.W(), ftw.W(), bits.W());
                __ Strh(ftw.W(), MemOperand(state, kFtw));

                __ Bind(&status);
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
            // Only the architectural sign bit changes. The significand and
            // binary64-canonical/reduced-ready provenance remain valid.
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


}  // namespace swift::runtime::backend::arm64
