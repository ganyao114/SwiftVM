#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/x87.h"

namespace swift::x86 {

#define __ assembler->

namespace {

bool IsStackRegister(const _Operand& operand) {
    return operand.type == O_REG && operand.index >= R_ST0 && operand.index <= R_ST7;
}

u8 StackIndex(const _Operand& operand) {
    return static_cast<u8>(operand.index - R_ST0);
}

bool IsMemoryOperand(const _Operand& operand) {
    return operand.type == O_MEM || operand.type == O_SMEM || operand.type == O_DISP;
}

X87Format FloatFormat(u16 bits) {
    switch (bits) {
        case 32: return X87Format::Float32;
        case 64: return X87Format::Float64;
        case 80: return X87Format::Float80;
        default: return X87Format::None;
    }
}

X87Format IntegerFormat(u16 bits) {
    switch (bits) {
        case 16: return X87Format::Int16;
        case 32: return X87Format::Int32;
        case 64: return X87Format::Int64;
        default: return X87Format::None;
    }
}

bool IsPopBinary(u16 opcode) {
    return opcode == I_FADDP || opcode == I_FMULP || opcode == I_FSUBP ||
           opcode == I_FSUBRP || opcode == I_FDIVP || opcode == I_FDIVRP;
}

bool IsReverseBinary(u16 opcode) {
    return opcode == I_FSUBR || opcode == I_FSUBRP || opcode == I_FDIVR ||
           opcode == I_FDIVRP || opcode == I_FISUBR || opcode == I_FIDIVR;
}

X87Binary BinaryOperation(u16 opcode) {
    switch (opcode) {
        case I_FADD:
        case I_FADDP:
        case I_FIADD:
            return X87Binary::Add;
        case I_FMUL:
        case I_FMULP:
        case I_FIMUL:
            return X87Binary::Mul;
        case I_FSUB:
        case I_FSUBR:
        case I_FSUBP:
        case I_FSUBRP:
        case I_FISUB:
        case I_FISUBR:
            return X87Binary::Sub;
        default:
            return X87Binary::Div;
    }
}

}  // namespace

// True for the x87 commands that dereference `guest_address`. Only those get
// the guest-fault check below; adding it to the register-only forms would put
// a live result register (and a test) on every FADD ST(i) for nothing.
static bool X87TouchesMemory(u64 command) {
    const auto action = static_cast<X87Action>(command & 0xFF);
    const auto format = static_cast<X87Format>((command >> 8) & 0xFF);
    if (format == X87Format::Register) {
        return false;
    }
    switch (action) {
        case X87Action::LoadFloat:
        case X87Action::StoreFloat:
        case X87Action::LoadInt:
        case X87Action::StoreInt:
        case X87Action::StoreControl:
        case X87Action::LoadControl:
        case X87Action::StoreStatus:
        case X87Action::StoreEnvironment:
        case X87Action::LoadEnvironment:
            return true;
        default:
            return false;
    }
}

// Turns a helper's kX87GuestFault bit into a guest-visible #PF.
//
// CheckMemoryAlignment is the runtime's generic "if (value & mask) exit the
// block with HaltReason::PageFatal" primitive (see EmitCheckMemoryAlignment /
// RunCheckMemoryAlignment); DecodeCmpxchg16b uses it for the unaligned-LOCK
// #GP. Reused here because a helper that refused to touch unmapped guest
// memory must fault the guest, not silently do nothing. SetLocation first so
// the halt reports the faulting instruction's rip and not the block entry.
void X64Decoder::RaiseIfGuestFault(ir::Value helper_result, u64 faulting_pc) {
    // kX87GuestFault and decoder_string.cc's kStringGuestFault are the same
    // bit by construction: one emitter serves every helper that has to
    // validate before it dereferences.
    static_assert(kX87GuestFault == (u64(1) << 63));
    __ SetLocation(ir::Lambda{ir::Imm{faulting_pc}});
    __ CheckMemoryAlignment(helper_result, ir::Imm(kX87GuestFault));
}

ir::Value X64Decoder::CallX87(u64 command, ir::Value guest_address) {
    auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
    ir::Value result;
    if (swift::runtime::GetSvmConfig().x87_jit) {
        // The mid-tier inlines some commands and zeroes the result on those
        // paths, so the fault bit can only ever come from its helper bailout.
        result = __ X87Op(context, ir::Imm(command), guest_address)
                         .SetType(ir::ValueType::U64);
    } else {
        auto encoded = __ LoadImm(ir::Imm(command)).SetType(ir::ValueType::U64);
        // Keep AFP OFF byte-identical: the dedicated target address is itself
        // a codegen-visible immediate even though EmitHostCall would ignore
        // the effect tag when AFP is inactive.
        const bool fp_free = sse_afp_nan_ &&
                             X87CommandFPCRTransparent(command);
        const auto target = fp_free ? &X87DispatchFPFree : &X87Dispatch;
        result = __ CallLambda(
                               ir::Lambda{
                                       ir::DataClass{ir::Imm{reinterpret_cast<VAddr>(target)}},
                                       ir::HelperCallTraits{
                                               .host_fp = fp_free
                                                       ? ir::HostFpEffect::FPCRTransparent
                                                       : ir::HostFpEffect::MayTouch,
                                       }},
                               context,
                               encoded,
                               guest_address)
                         .SetType(ir::ValueType::U64);
    }
    if (X87TouchesMemory(command)) {
        RaiseIfGuestFault(result, insn_pc);
    }
    return result;
}

void X64Decoder::ApplyX87CompareFlags(ir::Value flags) {
    // FCOMI/FUCOMI clear OF/SF/AF and define ZF/PF/CF in exactly the same
    // compact encoding used by VecFCmp.
    __ ClearFlags(ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::AuxiliaryCarry);
    auto one = __ LoadImm(ir::Imm(u64(1)));
    auto zero = __ LoadImm(ir::Imm(u64(0)));

    auto pf = __ And(__ LsrImm(flags, ir::Imm(1u)), ir::Operand{ir::Imm(u64(1))});
    auto parity_value = __ Select(__ TestNotZero(pf), zero, one);
    __ SaveFlags(__ Or(parity_value, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Parity);

    auto zf = __ And(__ LsrImm(flags, ir::Imm(2u)), ir::Operand{ir::Imm(u64(1))});
    auto zero_value = __ Select(__ TestNotZero(zf), zero, one);
    __ SaveFlags(__ Or(zero_value, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Zero);

    auto cf = __ And(flags, ir::Operand{ir::Imm(u64(1))});
    auto carry_value = __ Add(__ LoadImm(ir::Imm(~u64(0))), ir::Operand{cf});
    __ SaveFlags(carry_value, ir::Flags::Carry);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}

void X64Decoder::DecodeX87FreePop(u8 index) {
    CallX87(MakeX87Command(
                    X87Action::Free, X87Format::Register, index, 0, X87Pop),
            __ LoadImm(ir::Imm(u64(0))));
}

void X64Decoder::DecodeX87(_DInst& insn) {
    _Operand* memory_operand = nullptr;
    _Operand* stack_operand = nullptr;
    for (auto& operand : insn.ops) {
        if (!memory_operand && IsMemoryOperand(operand)) memory_operand = &operand;
        // Two-register forms spell both ST(0) and ST(i).  The encoded selector
        // (and the helper command's index) is the nonzero ST(i) operand.
        if (IsStackRegister(operand) &&
            (!stack_operand || StackIndex(*stack_operand) == 0)) {
            stack_operand = &operand;
        }
    }

    // Register-only x87 encodings that distorm reports without operands retain
    // their ST(i) selector in the low ModRM bits.
    const auto* bytes = reinterpret_cast<const u8*>(
            memory->GetPointer(reinterpret_cast<void*>(pc - insn.size)));
    const u8 raw_index = bytes ? static_cast<u8>(bytes[insn.size - 1] & 7) : 0;
    const u8 index = stack_operand ? StackIndex(*stack_operand) : raw_index;
    auto zero = [&] { return __ LoadImm(ir::Imm(u64(0))); };
    auto address = [&] {
        return memory_operand ? FlatAddress(insn, *memory_operand) : zero();
    };

    switch (insn.opcode) {
        case I_FLD: {
            if (memory_operand) {
                CallX87(MakeX87Command(X87Action::LoadFloat,
                                       FloatFormat(memory_operand->size)),
                        address());
            } else {
                CallX87(MakeX87Command(
                                X87Action::LoadReg, X87Format::Register, index),
                        zero());
            }
            break;
        }
        case I_FST:
        case I_FSTP: {
            const u32 flags = insn.opcode == I_FSTP ? X87Pop : 0;
            if (memory_operand) {
                CallX87(MakeX87Command(X87Action::StoreFloat,
                                       FloatFormat(memory_operand->size),
                                       0,
                                       0,
                                       flags),
                        address());
            } else {
                CallX87(MakeX87Command(
                                X87Action::StoreReg, X87Format::Register, index, 0, flags),
                        zero());
            }
            break;
        }
        case I_FILD:
            CallX87(MakeX87Command(X87Action::LoadInt,
                                   IntegerFormat(memory_operand->size)),
                    address());
            break;
        case I_FIST:
        case I_FISTP:
        case I_FISTTP: {
            u32 flags = insn.opcode != I_FIST ? X87Pop : 0;
            if (insn.opcode == I_FISTTP) flags |= X87Truncate;
            CallX87(MakeX87Command(X87Action::StoreInt,
                                   IntegerFormat(memory_operand->size),
                                   0,
                                   0,
                                   flags),
                    address());
            break;
        }

        case I_FADD:
        case I_FADDP:
        case I_FIADD:
        case I_FMUL:
        case I_FMULP:
        case I_FIMUL:
        case I_FSUB:
        case I_FSUBR:
        case I_FSUBP:
        case I_FSUBRP:
        case I_FISUB:
        case I_FISUBR:
        case I_FDIV:
        case I_FDIVR:
        case I_FDIVP:
        case I_FDIVRP:
        case I_FIDIV:
        case I_FIDIVR: {
            const bool integer = insn.opcode == I_FIADD || insn.opcode == I_FIMUL ||
                                 insn.opcode == I_FISUB || insn.opcode == I_FISUBR ||
                                 insn.opcode == I_FIDIV || insn.opcode == I_FIDIVR;
            X87Format format = X87Format::Register;
            if (memory_operand) {
                format = integer ? IntegerFormat(memory_operand->size)
                                 : FloatFormat(memory_operand->size);
            }
            u32 flags = 0;
            if (IsPopBinary(insn.opcode)) flags |= X87Pop | X87DestIndex;
            if (IsReverseBinary(insn.opcode)) flags |= X87Reverse;
            if (!memory_operand && !IsPopBinary(insn.opcode) &&
                IsStackRegister(insn.ops[0]) && StackIndex(insn.ops[0]) != 0) {
                flags |= X87DestIndex;
            }
            CallX87(MakeX87Command(X87Action::Binary,
                                   format,
                                   index,
                                   static_cast<u8>(BinaryOperation(insn.opcode)),
                                   flags),
                    address());
            break;
        }

        case I_FCOM:
        case I_FCOMP:
        case I_FCOMPP:
        case I_FUCOM:
        case I_FUCOMP:
        case I_FUCOMPP:
        case I_FICOM:
        case I_FICOMP:
        case I_FCOMI:
        case I_FCOMIP:
        case I_FUCOMI:
        case I_FUCOMIP: {
            const bool integer = insn.opcode == I_FICOM || insn.opcode == I_FICOMP;
            X87Format format = X87Format::Register;
            if (memory_operand) {
                format = integer ? IntegerFormat(memory_operand->size)
                                 : FloatFormat(memory_operand->size);
            }
            u32 flags = 0;
            if (insn.opcode == I_FUCOM || insn.opcode == I_FUCOMP ||
                insn.opcode == I_FUCOMPP || insn.opcode == I_FUCOMI ||
                insn.opcode == I_FUCOMIP) {
                flags |= X87Unordered;
            }
            if (insn.opcode == I_FCOMP || insn.opcode == I_FUCOMP ||
                insn.opcode == I_FICOMP || insn.opcode == I_FCOMIP ||
                insn.opcode == I_FUCOMIP) {
                flags |= X87Pop;
            }
            if (insn.opcode == I_FCOMPP || insn.opcode == I_FUCOMPP) {
                flags |= X87PopTwice;
            }
            const bool to_eflags = insn.opcode == I_FCOMI || insn.opcode == I_FCOMIP ||
                                   insn.opcode == I_FUCOMI ||
                                   insn.opcode == I_FUCOMIP;
            if (to_eflags) flags |= X87ToEFlags;
            auto result = CallX87(
                    MakeX87Command(X87Action::Compare, format, index, 0, flags),
                    address());
            if (to_eflags) ApplyX87CompareFlags(result);
            break;
        }

        case I_FCHS:
        case I_FABS:
        case I_FTST:
        case I_FXAM:
        case I_FSQRT:
        case I_FRNDINT: {
            X87Unary operation = X87Unary::ChangeSign;
            switch (insn.opcode) {
                case I_FABS: operation = X87Unary::Abs; break;
                case I_FTST: operation = X87Unary::Test; break;
                case I_FXAM: operation = X87Unary::Examine; break;
                case I_FSQRT: operation = X87Unary::Sqrt; break;
                case I_FRNDINT: operation = X87Unary::Round; break;
                default: break;
            }
            CallX87(MakeX87Command(X87Action::Unary,
                                   X87Format::None,
                                   0,
                                   static_cast<u8>(operation)),
                    zero());
            break;
        }

        case I_FPREM:
        case I_FPREM1:
            CallX87(MakeX87Command(
                            X87Action::Remainder,
                            X87Format::None,
                            0,
                            static_cast<u8>(insn.opcode == I_FPREM
                                                    ? X87Remainder::Truncate
                                                    : X87Remainder::Nearest)),
                    zero());
            break;
        case I_FSCALE:
            CallX87(MakeX87Command(X87Action::Scale), zero());
            break;
        case I_FXTRACT:
            CallX87(MakeX87Command(X87Action::Extract), zero());
            break;

        case I_FSIN:
        case I_FCOS:
        case I_FSINCOS:
        case I_FPTAN:
        case I_FPATAN:
        case I_FYL2X:
        case I_FYL2XP1:
        case I_F2XM1: {
            X87Transcendental operation = X87Transcendental::Sin;
            switch (insn.opcode) {
                case I_FCOS: operation = X87Transcendental::Cos; break;
                case I_FSINCOS: operation = X87Transcendental::SinCos; break;
                case I_FPTAN: operation = X87Transcendental::Tan; break;
                case I_FPATAN: operation = X87Transcendental::Atan; break;
                case I_FYL2X: operation = X87Transcendental::YLog2X; break;
                case I_FYL2XP1:
                    operation = X87Transcendental::YLog2XPlusOne;
                    break;
                case I_F2XM1:
                    operation = X87Transcendental::TwoToXMinusOne;
                    break;
                default: break;
            }
            CallX87(MakeX87Command(X87Action::Transcendental,
                                   X87Format::None,
                                   0,
                                   static_cast<u8>(operation)),
                    zero());
            break;
        }

        case I_FLD1:
        case I_FLDL2T:
        case I_FLDL2E:
        case I_FLDPI:
        case I_FLDLG2:
        case I_FLDLN2:
        case I_FLDZ: {
            X87Constant constant = X87Constant::Zero;
            switch (insn.opcode) {
                case I_FLD1: constant = X87Constant::One; break;
                case I_FLDL2T: constant = X87Constant::Log2Ten; break;
                case I_FLDL2E: constant = X87Constant::Log2E; break;
                case I_FLDPI: constant = X87Constant::Pi; break;
                case I_FLDLG2: constant = X87Constant::Log10Two; break;
                case I_FLDLN2: constant = X87Constant::LnTwo; break;
                default: break;
            }
            CallX87(MakeX87Command(X87Action::LoadConstant,
                                   X87Format::None,
                                   0,
                                   static_cast<u8>(constant)),
                    zero());
            break;
        }

        case I_FXCH:
            CallX87(MakeX87Command(
                            X87Action::Exchange, X87Format::Register, index),
                    zero());
            break;
        case I_FFREE:
            CallX87(
                    MakeX87Command(X87Action::Free, X87Format::Register, index),
                    zero());
            break;
        case I_FINCSTP:
        case I_FDECSTP:
            CallX87(MakeX87Command(X87Action::AdjustTop,
                                   X87Format::None,
                                   0,
                                   0,
                                   insn.opcode == I_FINCSTP ? X87IncrementTop : 0),
                    zero());
            break;

        case I_FNSTCW:
        case I_FSTCW:
            CallX87(MakeX87Command(X87Action::StoreControl), address());
            break;
        case I_FLDCW:
            CallX87(MakeX87Command(X87Action::LoadControl), address());
            break;
        case I_FNSTSW:
        case I_FSTSW: {
            if (insn.ops[0].type == O_REG) {
                auto status =
                        CallX87(MakeX87Command(
                                        X87Action::StoreStatus, X87Format::Register),
                                zero())
                                .SetType(ir::ValueType::U16);
                Dst(insn, insn.ops[0], status);
            } else {
                CallX87(MakeX87Command(X87Action::StoreStatus), address());
            }
            break;
        }
        case I_FNINIT:
        case I_FINIT:
            CallX87(MakeX87Command(X87Action::Init), zero());
            break;
        case I_FNCLEX:
        case I_FCLEX:
            CallX87(MakeX87Command(X87Action::ClearExceptions), zero());
            break;
        case I_FNSTENV:
        case I_FSTENV:
            CallX87(MakeX87Command(X87Action::StoreEnvironment), address());
            break;
        case I_FLDENV:
            CallX87(MakeX87Command(X87Action::LoadEnvironment), address());
            break;
        case I_FNOP:
        case I_WAIT:
            __ Nop();
            break;
        default:
            PANIC("Unexpected x87 opcode {}", insn.opcode);
    }
}

#undef __

}  // namespace swift::x86
