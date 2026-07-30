//
// x86 opcode-to-lowering dispatch.
//

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/xsave.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

#if defined(__GNUC__) || defined(__clang__)
#define SVM_DISPATCH_STAGE __attribute__((always_inline))
#else
#define SVM_DISPATCH_STAGE
#endif

bool X64Decoder::DecodeSwitch(_DInst& insn) {
    for (auto& op : insn.ops) {
        if (op.type != O_REG) {
            continue;
        }
        // Control / debug register moves are not modelled: trap gracefully
        // instead of panicking on the unknown register class.
        if ((op.index >= R_CR0 && op.index <= R_CR8) ||
            (op.index >= R_DR0 && op.index <= R_DR7)) {
            Interrupt(InterruptReason::ILL_CODE);
            return true;
        }
        // No MMX register file exists in this runtime, and MMX shares its
        // opcode numbers with SSE: distorm reports the same insn.opcode for
        // `paddb mm0,mm1` (0F FC) and `paddb xmm0,xmm1` (66 0F FC), with only
        // the operand register class telling them apart. Every handler below
        // reads that class as an XMM index, and x86_regs_table maps R_MM0..7
        // onto X86RegInfo::Xmm0..7 -- so an MMX-form instruction does not fail,
        // it quietly reads and writes the guest's XMM0-XMM7 and returns wrong
        // data. Roughly fifty shared opcodes are reachable this way (the whole
        // MMX/SSE2 integer set plus the SSSE3 additions and pextrw/pinsrw/
        // pmovmskb/movd/movq), which is why this is one blanket test at the
        // dispatch entry rather than a guard bolted onto each case.
        //
        // Refusing to decode returns false -> InterruptReason::FALLBACK ->
        // IllegalCode, killing the guest at the instruction. That is the
        // correct answer: MMX is not implemented, is not advertised (CPUID
        // leaf 1 EDX bit 23 is clear, so no correct program should emit it),
        // and dying loudly is strictly better than computing silently wrong
        // vectors. decoder_sse4.cc's SseXmmForm makes the same call for the
        // SSSE3 opcodes it owns.
        if (op.index >= R_MM0 && op.index <= R_MM7) {
            return false;
        }
    }
    return DecodeBaseOpcode(insn);
}

SVM_DISPATCH_STAGE bool X64Decoder::DecodeBaseOpcode(_DInst& insn) {
    switch (insn.opcode) {
        case I_NOP:
            __ Nop();
            break;
        case I_HLT:
            Interrupt(InterruptReason::HLT);
            break;
        case I_INT_3:
        case I_INT1:
        case I_INT:
            // With no guest IDT/kernel, software and debug interrupts surface
            // through the runtime's breakpoint/trap path.
            Interrupt(InterruptReason::BRK);
            break;
        case I_SYSCALL:
            // syscall: RCX = next RIP (pc already advanced), R11 = RFLAGS.
            R(_RegisterType::R_RCX, __ LoadImm(ir::Imm(pc)));
            R(_RegisterType::R_R11, __ LoadImm(ir::Imm(u64(0x202))));
            Interrupt(InterruptReason::SVC);
            break;
        case I_CPUID:
            DecodeCpuid(insn);
            break;
        case I_RDFSBASE:
            DecodeFsgsbase(insn, false, false);
            break;
        case I_RDGSBASE:
            DecodeFsgsbase(insn, false, true);
            break;
        case I_WRFSBASE:
            DecodeFsgsbase(insn, true, false);
            break;
        case I_WRGSBASE:
            DecodeFsgsbase(insn, true, true);
            break;
        case I_RDTSC:
            DecodeTimestamp(false);
            break;
        case I_RDTSCP:
            DecodeTimestamp(true);
            break;
        case I_RDRAND:
            DecodeRandom(insn);
            break;
        case I_MOVBE:
            DecodeMovbe(insn);
            break;
        case I_MOVNTI:
            DecodeMovnti(insn);
            break;
        case I_XLAT:
            DecodeXlat(insn);
            break;
        case I_XGETBV:
            EmitXgetbv(assembler, pc);
            break;
        case I_XSAVE:
        case I_XSAVE64:
            EmitXsave(assembler, FlatAddress(insn, insn.ops[0]), pc, insn_pc, false);
            break;
        case I_XSAVEOPT:
        case I_XSAVEOPT64:
            EmitXsaveopt(assembler, FlatAddress(insn, insn.ops[0]), pc, insn_pc);
            break;
        case I_XRSTOR:
        case I_XRSTOR64:
            EmitXsave(assembler, FlatAddress(insn, insn.ops[0]), pc, insn_pc, true);
            break;
        case I_UD2:
            Interrupt(InterruptReason::ILL_CODE);
            break;
        case I_CALL: {
            auto ret_type = is_64bit ? ir::ValueType::U64 : ir::ValueType::U32;
            // An indirect CALL resolves its target before pushing the return
            // address.  This matters for RSP-relative operands: resolving
            // call *disp(%rsp) after Push would read eight bytes below the
            // architectural target slot.  Keep the direct path unchanged so
            // its established IR/codegen fingerprint is unaffected.
            if (insn.ops[0].type != O_PC) {
                auto target = ir::Lambda{Src(insn, insn.ops[0])};
                Push(__ LoadImm(ir::Imm(pc)), ret_type);
                __ PushRSB(ir::Lambda(ir::Imm{pc}));
                __ SetLocation(target);
                __ ReturnToDispatcher();
                break;
            }
            Push(__ LoadImm(ir::Imm(pc)), ret_type);
            __ PushRSB(ir::Lambda(ir::Imm{pc}));
            DecodeCondJump(insn, Cond::AL);
            break;
        }
        case I_RET: {
            auto ret_addr = Pop(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            // ret imm16: also drop stack args
            if (insn.ops[0].type == O_IMM) {
                auto sp = R(_RegisterType::R_RSP);
                R(_RegisterType::R_RSP, __ Add(sp, ir::Operand{ir::Imm(u64(insn.imm.word))}));
            }
            __ SetLocation(ir::Lambda{ret_addr});
            __ PopRSB();
            __ Return();
            break;
        }
        case I_RETF: {
            auto ret_addr = Pop(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            __ SetLocation(ir::Lambda{ret_addr});
            __ PopRSB();
            __ Return();
            break;
        }
        case I_LEAVE: {
            R(_RegisterType::R_RSP, R(_RegisterType::R_RBP));
            auto rbp = Pop(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            R(is_64bit ? _RegisterType::R_RBP : _RegisterType::R_EBP, rbp);
            break;
        }
        case I_LEA:
            DecodeLea(insn);
            break;
        case I_JMP:
            DecodeCondJump(insn, Cond::AL);
            break;
        case I_JA:
            DecodeCondJump(insn, Cond::AT);
            break;
        case I_JAE:
            DecodeCondJump(insn, Cond::AE);
            break;
        case I_JB:
            DecodeCondJump(insn, Cond::BT);
            break;
        case I_JBE:
            DecodeCondJump(insn, Cond::BE);
            break;
        case I_JZ:
            DecodeCondJump(insn, Cond::EQ);
            break;
        case I_JNZ:
            DecodeCondJump(insn, Cond::NE);
            break;
        case I_JG:
            DecodeCondJump(insn, Cond::GT);
            break;
        case I_JGE:
            DecodeCondJump(insn, Cond::GE);
            break;
        case I_JL:
            DecodeCondJump(insn, Cond::LT);
            break;
        case I_JLE:
            DecodeCondJump(insn, Cond::LE);
            break;
        case I_JS:
            DecodeCondJump(insn, Cond::SN);
            break;
        case I_JNS:
            DecodeCondJump(insn, Cond::NS);
            break;
        case I_JP:
            DecodeCondJump(insn, Cond::PA);
            break;
        case I_JO:
            DecodeCondJump(insn, Cond::OF);
            break;
        case I_JNO:
            DecodeCondJump(insn, Cond::NO);
            break;
        case I_JNP:
            DecodeCondJump(insn, Cond::NP);
            break;
        case I_JCXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_CX);
            break;
        case I_JECXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_ECX);
            break;
        case I_JRCXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_RCX);
            break;
        case I_MOV:
            DecodeMov(insn);
            break;
        case I_MOVZX:
            DecodeMovzx(insn);
            break;
        case I_MOVSX:
        case I_MOVSXD:
            DecodeMovsx(insn);
            break;
        case I_MOVS:
            DecodeMovs(insn);
            break;
        case I_STOS:
            DecodeStos(insn);
            break;
        case I_LODS:
            DecodeLods(insn);
            break;
        case I_CMPS:
            DecodeCmps(insn);
            break;
        case I_SCAS:
            DecodeScas(insn);
            break;
        case I_CMOVA:
            DecodeCondMov(insn, Cond::AT);
            break;
        case I_CMOVAE:
            DecodeCondMov(insn, Cond::AE);
            break;
        case I_CMOVB:
            DecodeCondMov(insn, Cond::BT);
            break;
        case I_CMOVBE:
            DecodeCondMov(insn, Cond::BE);
            break;
        case I_CMOVZ:
            DecodeCondMov(insn, Cond::EQ);
            break;
        case I_CMOVG:
            DecodeCondMov(insn, Cond::GT);
            break;
        case I_CMOVGE:
            DecodeCondMov(insn, Cond::GE);
            break;
        case I_CMOVL:
            DecodeCondMov(insn, Cond::LT);
            break;
        case I_CMOVLE:
            DecodeCondMov(insn, Cond::LE);
            break;
        case I_CMOVNZ:
            DecodeCondMov(insn, Cond::NE);
            break;
        case I_CMOVNO:
            DecodeCondMov(insn, Cond::NO);
            break;
        case I_CMOVO:
            DecodeCondMov(insn, Cond::OF);
            break;
        case I_CMOVP:
            DecodeCondMov(insn, Cond::PA);
            break;
        case I_CMOVNP:
            DecodeCondMov(insn, Cond::NP);
            break;
        case I_CMOVS:
            DecodeCondMov(insn, Cond::SN);
            break;
        case I_CMOVNS:
            DecodeCondMov(insn, Cond::NS);
            break;
        case I_SETA:
            DecodeSetCC(insn, Cond::AT);
            break;
        case I_SETAE:
            DecodeSetCC(insn, Cond::AE);
            break;
        case I_SETB:
            DecodeSetCC(insn, Cond::BT);
            break;
        case I_SETBE:
            DecodeSetCC(insn, Cond::BE);
            break;
        case I_SETG:
            DecodeSetCC(insn, Cond::GT);
            break;
        case I_SETGE:
            DecodeSetCC(insn, Cond::GE);
            break;
        case I_SETL:
            DecodeSetCC(insn, Cond::LT);
            break;
        case I_SETLE:
            DecodeSetCC(insn, Cond::LE);
            break;
        case I_SETNO:
            DecodeSetCC(insn, Cond::NO);
            break;
        case I_SETNP:
            DecodeSetCC(insn, Cond::NP);
            break;
        case I_SETNS:
            DecodeSetCC(insn, Cond::NS);
            break;
        case I_SETNZ:
            DecodeSetCC(insn, Cond::NE);
            break;
        case I_SETO:
            DecodeSetCC(insn, Cond::OF);
            break;
        case I_SETP:
            DecodeSetCC(insn, Cond::PA);
            break;
        case I_SETS:
            DecodeSetCC(insn, Cond::SN);
            break;
        case I_SETZ:
            DecodeSetCC(insn, Cond::EQ);
            break;
        case I_ADD:
            DecodeAddSub(insn, false);
            break;
        case I_XADD:
            DecodeAddSub(insn, false, true, true);
            break;
        case I_SUB:
            DecodeAddSub(insn, true);
            break;
        case I_CMP:
            DecodeAddSub(insn, true, false);
            break;
        case I_ADC:
            DecodeAddSubWithCarry(insn, false);
            break;
        case I_SBB:
            DecodeAddSubWithCarry(insn, true);
            break;
        case I_INC:
            DecodeIncAndDec(insn, false);
            break;
        case I_DEC:
            DecodeIncAndDec(insn, true);
            break;
        case I_NEG:
            DecodeNeg(insn);
            break;
        case I_NOT:
            DecodeNot(insn);
            break;
        case I_XCHG:
            DecodeXchg(insn);
            break;
        case I_MUL:
            DecodeMulOneOperand(insn, false);
            break;
        case I_IMUL:
            DecodeIMul(insn);
            break;
        case I_DIV:
            DecodeDiv(insn, false);
            break;
        case I_IDIV:
            DecodeDiv(insn, true);
            break;
        case I_OR:
            DecodeOr(insn);
            break;
        case I_AND:
            DecodeAnd(insn, true);
            break;
        case I_TEST:
            DecodeAnd(insn, false);
            break;
        case I_XOR:
            DecodeXor(insn);
            break;
        case I_SHL:
        case I_SAL:
            DecodeShlShr(insn, false);
            break;
        case I_SHR:
            DecodeShlShr(insn, true);
            break;
        case I_SAR:
            DecodeSar(insn);
            break;
        case I_SHLD:
            DecodeDoubleShift(insn, false);
            break;
        case I_SHRD:
            DecodeDoubleShift(insn, true);
            break;
        case I_RCL:
            DecodeRotateCarry(insn, true);
            break;
        case I_RCR:
            DecodeRotateCarry(insn, false);
            break;
        case I_CLC:
            __ SetCarry(__ LoadImm(ir::Imm(u8(0))));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
            break;
        case I_STC:
            __ SetCarry(__ LoadImm(ir::Imm(u8(1))));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
            break;
        case I_CMC:
            __ SetCarry(__ Xor(CarryValue(), ir::Operand{ir::Imm(u8(1))}));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
            break;
        case I_CLD:
            StoreDirection(false);
            break;
        case I_STD:
            StoreDirection(true);
            break;
        case I_PUSH:
            DecodePush(insn);
            break;
        case I_POP:
            DecodePop(insn);
            break;
        case I_PUSHF:
            DecodePushf(insn);
            break;
        case I_POPF:
            DecodePopf(insn);
            break;
        case I_PUSHA:
            DecodePushA(insn);
            break;
        case I_POPA:
            DecodePopA(insn);
            break;
        case I_CBW: {
            auto al = R(_RegisterType::R_AL);
            R(_RegisterType::R_AX, __ SignExtend(al).SetType(ir::ValueType::S16));
            break;
        }
        case I_LAHF: {
            // AH = SF:ZF:0:AF:0:PF:1:CF
            auto cf = CheckCond(Cond::BT);
            auto pf = CheckCond(Cond::PA);
            auto af = __ TestFlags(ir::Flags::AuxiliaryCarry).SetType(ir::ValueType::U8);
            // TestFlags reads the guest flag shadow directly.  CondSelect
            // would observe whatever host NZCV the preceding PF/AF query
            // happened to leave behind (notably after UCOMIS*).
            auto zf = __ TestFlags(ir::Flags::Zero).SetType(ir::ValueType::U8);
            auto sf = __ TestFlags(ir::Flags::Negate).SetType(ir::ValueType::U8);
            auto lo = __ Or(cf, ir::Operand{__ LslImm(pf, ir::Imm(2u))});
            auto mid = __ Or(__ LslImm(af, ir::Imm(4u)), ir::Operand{__ LslImm(zf, ir::Imm(6u))});
            auto ah = __ Or(
                    __ Or(lo, ir::Operand{mid}),
                    ir::Operand{__ Or(__ LslImm(sf, ir::Imm(7u)), ir::Operand{ir::Imm(u64(2))})});
            R(_RegisterType::R_AH, ah);
            break;
        }
        case I_SAHF: {
            // SF:ZF:0:AF:0:PF:1:CF <- AH.  Save each independently because
            // SAHF permits combinations (notably SF+ZF) that no single ALU
            // result can represent.
            auto ah = R(_RegisterType::R_AH).SetType(ir::ValueType::U64);
            auto bit = [&](u32 position) {
                return __ And(__ LsrImm(ah, ir::Imm(position)),
                              ir::Operand{ir::Imm(u64(1))});
            };
            auto one = __ LoadImm(ir::Imm(u64(1)));
            auto zero = __ LoadImm(ir::Imm(u64(0)));

            auto pf = bit(2);
            auto parity_value = __ Select(__ TestNotZero(pf), zero, one);
            __ SaveFlags(__ Or(parity_value, ir::Operand{ir::Imm(u64(0))}),
                         ir::Flags::Parity);

            auto zf = bit(6);
            auto zero_value = __ Select(__ TestNotZero(zf), zero, one);
            __ SaveFlags(__ Or(zero_value, ir::Operand{ir::Imm(u64(0))}),
                         ir::Flags::Zero);

            auto sf = bit(7);
            auto sign_value = __ LslImm(sf, ir::Imm(63u)).SetType(ir::ValueType::U64);
            __ SaveFlags(__ Or(sign_value, ir::Operand{ir::Imm(u64(0))}),
                         ir::Flags::Negate);

            // 0xF + AFbit has an auxiliary carry exactly when AFbit is one.
            auto auxiliary_value =
                    __ Add(__ LoadImm(ir::Imm(u64(0xF))), ir::Operand{bit(4)});
            __ SaveFlags(auxiliary_value, ir::Flags::AuxiliaryCarry);

            __ SetCarry(bit(0));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
            break;
        }
        case I_CWDE: {
            auto ax = R(_RegisterType::R_AX);
            R(_RegisterType::R_EAX, __ SignExtend(ax).SetType(ir::ValueType::S32));
            break;
        }
        case I_CDQE: {
            auto eax = R(_RegisterType::R_EAX);
            R(_RegisterType::R_RAX, __ SignExtend(eax).SetType(ir::ValueType::U64));
            break;
        }
        case I_CWD: {
            auto ax = __ SignExtend(R(_RegisterType::R_AX)).SetType(ir::ValueType::S32);
            R(_RegisterType::R_DX, __ AsrImm(ax, ir::Imm(15u)));
            break;
        }
        case I_CDQ: {
            auto eax = __ SignExtend(R(_RegisterType::R_EAX)).SetType(ir::ValueType::U64);
            R(_RegisterType::R_EDX, __ AsrImm(eax, ir::Imm(31u)));
            break;
        }
        case I_CQO: {
            auto rax = R(_RegisterType::R_RAX).SetType(ir::ValueType::U64);
            R(_RegisterType::R_RDX, __ AsrImm(rax, ir::Imm(63u)));
            break;
        }
        default:
            return DecodeX87Opcode(insn);
    }
    return true;
}

SVM_DISPATCH_STAGE bool X64Decoder::DecodeX87Opcode(_DInst& insn) {
    switch (insn.opcode) {
        // ---- x87 floating-point stack ----------------------------------
        case I_FLD:
        case I_FST:
        case I_FSTP:
        case I_FILD:
        case I_FIST:
        case I_FISTP:
        case I_FISTTP:
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
        case I_FIDIVR:
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
        case I_FUCOMIP:
        case I_FCHS:
        case I_FABS:
        case I_FTST:
        case I_FXAM:
        case I_FSQRT:
        case I_FRNDINT:
        case I_FPREM:
        case I_FPREM1:
        case I_FSCALE:
        case I_FXTRACT:
        case I_FSIN:
        case I_FCOS:
        case I_FSINCOS:
        case I_FPTAN:
        case I_FPATAN:
        case I_FYL2X:
        case I_FYL2XP1:
        case I_F2XM1:
        case I_FLD1:
        case I_FLDL2T:
        case I_FLDL2E:
        case I_FLDPI:
        case I_FLDLG2:
        case I_FLDLN2:
        case I_FLDZ:
        case I_FXCH:
        case I_FFREE:
        case I_FINCSTP:
        case I_FDECSTP:
        case I_FNSTCW:
        case I_FSTCW:
        case I_FLDCW:
        case I_FNSTSW:
        case I_FSTSW:
        case I_FNINIT:
        case I_FINIT:
        case I_FNCLEX:
        case I_FCLEX:
        case I_FNSTENV:
        case I_FSTENV:
        case I_FLDENV:
        case I_FNOP:
        case I_WAIT:
            DecodeX87(insn);
            break;
        default:
            return DecodeSseOpcode(insn);
    }
    return true;
}

SVM_DISPATCH_STAGE bool X64Decoder::DecodeSseOpcode(_DInst& insn) {
    switch (insn.opcode) {
        // ---- SSE subset (glibc baseline SSE2 string routines) ----
        case I_MOVD:
            DecodeMovd(insn);
            break;
        case I_MOVQ:
            DecodeMovq(insn);
            break;
        case I_MOVDQA:
        case I_MOVDQU:
        case I_MOVAPS:
        case I_MOVUPS:
        case I_MOVAPD:
        case I_MOVUPD:
        case I_MOVNTDQ:
        case I_MOVNTPS:
        case I_MOVNTPD:
        case I_LDDQU:
            DecodeMovVec(insn);
            break;
        case I_MOVSD:
            DecodeMovsd(insn);
            break;
        case I_MOVSS:
            DecodeMovss(insn);
            break;
        case I_MOVLPD:
        case I_MOVLPS:
            DecodeMovHalf(insn, false);
            break;
        case I_MOVHPD:
        case I_MOVHPS:
            DecodeMovHalf(insn, true);
            break;
        case I_MOVHLPS:
            DecodeMovhlps(insn, false);
            break;
        case I_MOVLHPS:
            DecodeMovhlps(insn, true);
            break;
        case I_MOVMSKPS:
            DecodeMovmsk(insn, false);
            break;
        case I_MOVMSKPD:
            DecodeMovmsk(insn, true);
            break;
        case I_PXOR:
        case I_XORPS:
        case I_XORPD:
            DecodeVecBitwise(insn, VecBitwiseOp::Xor);
            break;
        case I_POR:
        case I_ORPS:
        case I_ORPD:
            DecodeVecBitwise(insn, VecBitwiseOp::Or);
            break;
        case I_PAND:
        case I_ANDPS:
        case I_ANDPD:
            DecodeVecBitwise(insn, VecBitwiseOp::And);
            break;
        case I_PANDN:
        case I_ANDNPS:
        case I_ANDNPD:
            DecodeVecBitwise(insn, VecBitwiseOp::AndNot);
            break;
        case I_PADDQ:
            // The one shared opcode the register-class test at the top of this
            // function cannot see. This distorm snapshot decodes BOTH forms of
            // PADDQ -- 0F D4 (MMX, mm/m64) and 66 0F D4 (SSE2, xmm/m128) -- with
            // XMM operand indices, identical flags, identical opsize and an
            // empty unusedPrefixesMask; nothing in the _DInst separates them.
            // Sweeping all 88 MMX-form encodings against this snapshot showed
            // it is the only one with that defect, so it gets the only special
            // case: read the mandatory prefix off the encoding instead.
            if (!HasOperandSizePrefix()) {
                return false;  // MMX form -> FALLBACK -> IllegalCode
            }
            DecodeVecInt(insn, VecIntOp::Add, 64);
            break;
        case I_PSUBQ:
            DecodeVecInt(insn, VecIntOp::Sub, 64);
            break;
        case I_PUNPCKLDQ:
            DecodeVecZip(insn, 32, false);
            break;
        case I_PUNPCKHDQ:
            DecodeVecZip(insn, 32, true);
            break;
        case I_PUNPCKLQDQ:
            DecodeVecZip(insn, 64, false);
            break;
        case I_PUNPCKHQDQ:
            DecodeVecZip(insn, 64, true);
            break;
        case I_PMULUDQ:
            DecodeSseMulWiden(insn, false);
            break;
        case I_PMULDQ:
            DecodeSseMulWiden(insn, true);
            break;
        case I_PADDB:
            DecodeVecInt(insn, VecIntOp::Add, 8);
            break;
        case I_PSUBB:
            DecodeVecInt(insn, VecIntOp::Sub, 8);
            break;
        case I_PADDW:
            DecodeVecInt(insn, VecIntOp::Add, 16);
            break;
        case I_PSUBW:
            DecodeVecInt(insn, VecIntOp::Sub, 16);
            break;
        case I_PADDD:
            DecodeVec4Add(insn);
            break;
        case I_PSUBD:
            DecodeVecInt(insn, VecIntOp::Sub, 32);
            break;
        case I_PADDSB: DecodeVecSat(insn, false, 8, true); break;
        case I_PADDSW: DecodeVecSat(insn, false, 16, true); break;
        case I_PADDUSB: DecodeVecSat(insn, false, 8, false); break;
        case I_PADDUSW: DecodeVecSat(insn, false, 16, false); break;
        case I_PSUBSB: DecodeVecSat(insn, true, 8, true); break;
        case I_PSUBSW: DecodeVecSat(insn, true, 16, true); break;
        case I_PSUBUSB: DecodeVecSat(insn, true, 8, false); break;
        case I_PSUBUSW: DecodeVecSat(insn, true, 16, false); break;
        case I_PACKSSWB: DecodeVecPack(insn, 16, false); break;
        case I_PACKSSDW: DecodeVecPack(insn, 32, false); break;
        case I_PACKUSWB: DecodeVecPack(insn, 16, true); break;
        case I_PCMPEQB:
            DecodeVecInt(insn, VecIntOp::CmpEq, 8);
            break;
        case I_PCMPEQW:
            DecodeVecInt(insn, VecIntOp::CmpEq, 16);
            break;
        case I_PCMPEQD:
            DecodeVecInt(insn, VecIntOp::CmpEq, 32);
            break;
        case I_PCMPGTB:
            DecodeVecInt(insn, VecIntOp::CmpGt, 8);
            break;
        case I_PCMPGTW:
            DecodeVecInt(insn, VecIntOp::CmpGt, 16);
            break;
        case I_PCMPGTD:
            DecodeVecInt(insn, VecIntOp::CmpGt, 32);
            break;
        case I_PMINUB:
            DecodeVecMinMax(insn, false, 8, false);
            break;
        case I_PMAXUB:
            DecodeVecMinMax(insn, true, 8, false);
            break;
        case I_PMINUD:
            DecodeVecMinMax(insn, false, 32, false);
            break;
        case I_PMAXUD:
            DecodeVecMinMax(insn, true, 32, false);
            break;
        case I_PMINSW:
            DecodeVecMinMax(insn, false, 16, true);
            break;
        case I_PMAXSW:
            DecodeVecMinMax(insn, true, 16, true);
            break;
        case I_PAVGB:
            DecodeVecAvg(insn, 8);
            break;
        case I_PAVGW:
            DecodeVecAvg(insn, 16);
            break;
        case I_PSADBW:
            DecodeVecAbsDiffSum8(insn);
            break;
        case I_PUNPCKLBW:
            DecodeVecZip(insn, 8, false);
            break;
        case I_PUNPCKHBW:
            DecodeVecZip(insn, 8, true);
            break;
        case I_PUNPCKLWD:
            DecodeVecZip(insn, 16, false);
            break;
        case I_PUNPCKHWD:
            DecodeVecZip(insn, 16, true);
            break;
        case I_UNPCKLPS:
            DecodeVecZip(insn, 32, false);
            break;
        case I_UNPCKHPS:
            DecodeVecZip(insn, 32, true);
            break;
        case I_UNPCKLPD:
            DecodeVecZip(insn, 64, false);
            break;
        case I_UNPCKHPD:
            DecodeVecZip(insn, 64, true);
            break;
        case I_PSHUFD:
            DecodePshufd(insn);
            break;
        case I_SHUFPS:
            DecodeShufps(insn, false);
            break;
        case I_SHUFPD:
            DecodeShufps(insn, true);
            break;
        case I_PSLLDQ:
            DecodePshiftDQ(insn, true);
            break;
        case I_PSRLDQ:
            DecodePshiftDQ(insn, false);
            break;
        case I_PSLLW:
            DecodePshift(insn, true, 0);
            break;
        case I_PSLLD:
            DecodePshift(insn, true, 1);
            break;
        case I_PSLLQ:
            DecodePshift(insn, true, 2);
            break;
        case I_PSRLW:
            DecodePshift(insn, false, 0);
            break;
        case I_PSRLD:
            DecodePshift(insn, false, 1);
            break;
        case I_PSRLQ:
            DecodePshift(insn, false, 2);
            break;
        case I_PSRAW:
            DecodePshiftA(insn, 0);
            break;
        case I_PSRAD:
            DecodePshiftA(insn, 1);
            break;
        default:
            return DecodeSseFloatOpcode(insn);
    }
    return true;
}

SVM_DISPATCH_STAGE bool X64Decoder::DecodeSseFloatOpcode(_DInst& insn) {
    switch (insn.opcode) {
        case I_ADDPS:
            DecodePackedFloatOp(insn, VecFloatOp::Add, 32);
            break;
        case I_ADDPD:
            DecodePackedFloatOp(insn, VecFloatOp::Add, 64);
            break;
        case I_SUBPS:
            DecodePackedFloatOp(insn, VecFloatOp::Sub, 32);
            break;
        case I_SUBPD:
            DecodePackedFloatOp(insn, VecFloatOp::Sub, 64);
            break;
        case I_MULPS:
            DecodePackedFloatOp(insn, VecFloatOp::Mul, 32);
            break;
        case I_MULPD:
            DecodePackedFloatOp(insn, VecFloatOp::Mul, 64);
            break;
        case I_DIVPS:
            DecodePackedFloatOp(insn, VecFloatOp::Div, 32);
            break;
        case I_DIVPD:
            DecodePackedFloatOp(insn, VecFloatOp::Div, 64);
            break;
        case I_ADDSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Add);
            break;
        case I_SUBSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Sub);
            break;
        case I_MULSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Mul);
            break;
        case I_DIVSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Div);
            break;
        case I_ADDSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Add, 64);
            break;
        case I_SUBSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Sub, 64);
            break;
        case I_MULSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Mul, 64);
            break;
        case I_DIVSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Div, 64);
            break;
        case I_CMPEQPS: DecodeFloatCompareMask(insn, 32, 0, false); break;
        case I_CMPLTPS: DecodeFloatCompareMask(insn, 32, 1, false); break;
        case I_CMPLEPS: DecodeFloatCompareMask(insn, 32, 2, false); break;
        case I_CMPUNORDPS: DecodeFloatCompareMask(insn, 32, 3, false); break;
        case I_CMPNEQPS: DecodeFloatCompareMask(insn, 32, 4, false); break;
        case I_CMPNLTPS: DecodeFloatCompareMask(insn, 32, 5, false); break;
        case I_CMPNLEPS: DecodeFloatCompareMask(insn, 32, 6, false); break;
        case I_CMPORDPS: DecodeFloatCompareMask(insn, 32, 7, false); break;
        case I_CMPEQPD: DecodeFloatCompareMask(insn, 64, 0, false); break;
        case I_CMPLTPD: DecodeFloatCompareMask(insn, 64, 1, false); break;
        case I_CMPLEPD: DecodeFloatCompareMask(insn, 64, 2, false); break;
        case I_CMPUNORDPD: DecodeFloatCompareMask(insn, 64, 3, false); break;
        case I_CMPNEQPD: DecodeFloatCompareMask(insn, 64, 4, false); break;
        case I_CMPNLTPD: DecodeFloatCompareMask(insn, 64, 5, false); break;
        case I_CMPNLEPD: DecodeFloatCompareMask(insn, 64, 6, false); break;
        case I_CMPORDPD: DecodeFloatCompareMask(insn, 64, 7, false); break;
        case I_CMPEQSS: DecodeFloatCompareMask(insn, 32, 0, true); break;
        case I_CMPLTSS: DecodeFloatCompareMask(insn, 32, 1, true); break;
        case I_CMPLESS: DecodeFloatCompareMask(insn, 32, 2, true); break;
        case I_CMPUNORDSS: DecodeFloatCompareMask(insn, 32, 3, true); break;
        case I_CMPNEQSS: DecodeFloatCompareMask(insn, 32, 4, true); break;
        case I_CMPNLTSS: DecodeFloatCompareMask(insn, 32, 5, true); break;
        case I_CMPNLESS: DecodeFloatCompareMask(insn, 32, 6, true); break;
        case I_CMPORDSS: DecodeFloatCompareMask(insn, 32, 7, true); break;
        case I_CMPEQSD: DecodeFloatCompareMask(insn, 64, 0, true); break;
        case I_CMPLTSD: DecodeFloatCompareMask(insn, 64, 1, true); break;
        case I_CMPLESD: DecodeFloatCompareMask(insn, 64, 2, true); break;
        case I_CMPUNORDSD: DecodeFloatCompareMask(insn, 64, 3, true); break;
        case I_CMPNEQSD: DecodeFloatCompareMask(insn, 64, 4, true); break;
        case I_CMPNLTSD: DecodeFloatCompareMask(insn, 64, 5, true); break;
        case I_CMPNLESD: DecodeFloatCompareMask(insn, 64, 6, true); break;
        case I_CMPORDSD: DecodeFloatCompareMask(insn, 64, 7, true); break;
        case I_MINPS: DecodeFloatMinMax(insn, 32, false, false); break;
        case I_MAXPS: DecodeFloatMinMax(insn, 32, true, false); break;
        case I_MINPD: DecodeFloatMinMax(insn, 64, false, false); break;
        case I_MAXPD: DecodeFloatMinMax(insn, 64, true, false); break;
        case I_MINSS: DecodeFloatMinMax(insn, 32, false, true); break;
        case I_MAXSS: DecodeFloatMinMax(insn, 32, true, true); break;
        case I_MINSD: DecodeFloatMinMax(insn, 64, false, true); break;
        case I_MAXSD: DecodeFloatMinMax(insn, 64, true, true); break;
        case I_SQRTPS: DecodeFloatUnary(insn, 32, 0, false); break;
        case I_SQRTPD: DecodeFloatUnary(insn, 64, 0, false); break;
        case I_SQRTSS: DecodeFloatUnary(insn, 32, 0, true); break;
        case I_SQRTSD: DecodeFloatUnary(insn, 64, 0, true); break;
        case I_RCPPS: DecodeFloatUnary(insn, 32, 1, false); break;
        case I_RCPSS: DecodeFloatUnary(insn, 32, 1, true); break;
        case I_RSQRTPS: DecodeFloatUnary(insn, 32, 2, false); break;
        case I_RSQRTSS: DecodeFloatUnary(insn, 32, 2, true); break;
        case I_PMADDWD:
            DecodeVecMadd16(insn);
            break;
        default:
            return DecodeSseMiscOpcode(insn);
    }
    return true;
}

SVM_DISPATCH_STAGE bool X64Decoder::DecodeSseMiscOpcode(_DInst& insn) {
    switch (insn.opcode) {
        case I_MOVSHDUP:
            DecodeVecDupPairs32(insn, true);
            break;
        case I_MOVSLDUP:
            DecodeVecDupPairs32(insn, false);
            break;
        case I_MOVDDUP:
            DecodeMovddup(insn);
            break;
        // I_HADDPS / I_HSUBPS are deliberately NOT listed here: they fall
        // through to `default:` -> DecodeSse4, alongside haddpd/hsubpd. The
        // host-lambda implementation that used to sit here flipped the sign
        // bit of a NaN result (clang lowers the pair of float subtractions to
        // FNEG + FADDP at -O2), so it was replaced by the pure-IR horizontal
        // path in decoder_sse4.cc rather than patched.
        case I_PEXTRW:
            DecodePextrw(insn);
            break;
        case I_PINSRW:
            DecodePinsrw(insn);
            break;
        case I_PMULLW:
            DecodeVecMul(insn, 16);
            break;
        case I_AESENC:
            DecodeAes(insn, 0);
            break;
        case I_AESENCLAST:
            DecodeAes(insn, 1);
            break;
        case I_AESDEC:
            DecodeAes(insn, 2);
            break;
        case I_AESDECLAST:
            DecodeAes(insn, 3);
            break;
        case I_AESKEYGENASSIST:
            DecodeAes(insn, 4);
            break;
        case I_PCLMULQDQ:
            DecodePclmul(insn);
            break;
        case I_PMULHW:
            DecodeVecMulHigh16(insn, true);
            break;
        case I_PMULHUW:
            DecodeVecMulHigh16(insn, false);
            break;
        case I_MASKMOVDQU:
            DecodeMaskmovdqu(insn);
            break;
        case I_PSHUFLW:
            DecodePshufw(insn, false);
            break;
        case I_PSHUFHW:
            DecodePshufw(insn, true);
            break;
        case I_CVTSI2SS:
            DecodeCvtsi2ss(insn);
            break;
        case I_CVTSI2SD:
            DecodeCvtsi2sd(insn);
            break;
        case I_CVTTSS2SI:
            DecodeCvttss2si(insn);
            break;
        case I_CVTTSD2SI:
            DecodeCvttsd2si(insn);
            break;
        case I_CVTSS2SI:
            DecodeCvtFloatToInt(insn, 32);
            break;
        case I_CVTSD2SI:
            DecodeCvtFloatToInt(insn, 64);
            break;
        case I_CVTDQ2PS: DecodePackedConvert(insn, 0); break;
        case I_CVTDQ2PD: DecodePackedConvert(insn, 1); break;
        case I_CVTPS2DQ: DecodePackedConvert(insn, 2); break;
        case I_CVTTPS2DQ: DecodePackedConvert(insn, 3); break;
        case I_CVTPD2DQ: DecodePackedConvert(insn, 4); break;
        case I_CVTTPD2DQ: DecodePackedConvert(insn, 5); break;
        case I_CVTPS2PD: DecodePackedConvert(insn, 6); break;
        case I_CVTPD2PS: DecodePackedConvert(insn, 7); break;
        case I_CVTSD2SS:
            DecodeCvtsd2ss(insn);
            break;
        case I_CVTSS2SD:
            DecodeCvtss2sd(insn);
            break;
        default:
            return DecodeExtendedOpcode(insn);
    }
    return true;
}

SVM_DISPATCH_STAGE bool X64Decoder::DecodeExtendedOpcode(_DInst& insn) {
    switch (insn.opcode) {
        case I_POPCNT:
            DecodePopcnt(insn);
            break;
        case I_BSWAP:
            DecodeBswap(insn);
            break;
        case I_LZCNT:
            // With the BMI gate on this adds the CF that the plain LZCNT path
            // omits; with it off it falls through to today's behaviour.
            DecodeLzcntBmi(insn);
            break;
        case I_CRC32:
            DecodeCrc32(insn);
            break;
        case I_LOOP:
        case I_LOOPZ:
        case I_LOOPNZ:
            DecodeLoop(insn);
            break;
        case I_ENTER:
            DecodeEnter(insn);
            break;
        case I_CMPXCHG8B:
            DecodeCmpxchg8b(insn);
            break;
        case I_CMPXCHG16B:
            DecodeCmpxchg16b(insn);
            break;
        case I_PALIGNR:
            DecodePalignr(insn);
            break;
        case I_PSHUFB:
            DecodePshufb(insn);
            break;
        case I_PMOVMSKB:
            DecodePmovmskb(insn);
            break;
        case I_STMXCSR:
            DecodeMxcsr(insn, false);
            break;
        case I_LDMXCSR:
            DecodeMxcsr(insn, true);
            break;
        case I_FXSAVE:
        case I_FXSAVE64:
            DecodeFxsave(insn, false);
            break;
        case I_FXRSTOR:
        case I_FXRSTOR64:
            DecodeFxsave(insn, true);
            break;
        case I_UCOMISD:
        case I_COMISD:
            DecodeUcomisd(insn);
            break;
        case I_UCOMISS:
        case I_COMISS:
            DecodeUcomis(insn, 32);
            break;
        case I_BSF:
            DecodeBitScan(insn, false);
            break;
        case I_TZCNT:
            // TZCNT and BSF differ in their zero-source behaviour and in CF/ZF,
            // so they cannot share a handler once BMI1 is advertised. With the
            // gate off DecodeTzcnt reproduces the old aliasing: tzcnt executes
            // as bsf on a CPU that hides BMI1.
            DecodeTzcnt(insn);
            break;
        case I_BSR:
            DecodeBitScan(insn, true);
            break;
        case I_CMPXCHG:
            DecodeCmpxchg(insn);
            break;
        case I_ROL:
            DecodeRotate(insn, true);
            break;
        case I_ROR:
            DecodeRotate(insn, false);
            break;
        case I_BT:
            DecodeBt(insn, 0);
            break;
        case I_BTS:
            DecodeBt(insn, 1);
            break;
        case I_BTR:
            DecodeBt(insn, 2);
            break;
        case I_BTC:
            DecodeBt(insn, 3);
            break;
        case I_PAUSE:
        case I_PREFETCHT0:
        case I_PREFETCHT1:
        case I_PREFETCHT2:
        case I_PREFETCHNTA:
        case I_PREFETCH:
        case I_PREFETCHW:
        case I_LFENCE:
        case I_MFENCE:
        case I_SFENCE:
        case I_EMMS:
        case I_CLFLUSH:
            // Timing / ordering hints only: no observable state in this
            // single-threaded model.
            __ Nop();
            break;
        // ---- AVX: VEX-encoded forms ----
        // One entry point so the SVM_AVX gate and the VEX.L / operand-shape
        // checks are written once; see DecodeAvx.
        case I_VMOVDQA:
        case I_VMOVDQU:
        case I_VMOVAPS:
        case I_VMOVUPS:
        case I_VMOVAPD:
        case I_VMOVUPD:
        case I_VMOVNTDQ:
        case I_VMOVNTDQA:
        case I_VMOVNTPS:
        case I_VMOVNTPD:
        case I_VLDDQU:
        case I_VMOVD:
        case I_VMOVQ:
        case I_VPXOR:
        case I_VPOR:
        case I_VPAND:
        case I_VPANDN:
        case I_VPADDB:
        case I_VPADDW:
        case I_VPADDD:
        case I_VPADDQ:
        case I_VPSUBB:
        case I_VPSUBW:
        case I_VPSUBD:
        case I_VPSUBQ:
        case I_VPCMPEQB:
        case I_VPCMPEQW:
        case I_VPCMPEQD:
        case I_VPCMPGTB:
        case I_VPCMPGTW:
        case I_VPCMPGTD:
        // Routed for their VEX.256 handlers (decoder_avx.cc). The VEX.128
        // forms have no handler yet; DecodeAvx's L=0 path declines them, which
        // traps the block as FALLBACK — the same outcome as before they were
        // listed here, never a mis-execution.
        case I_VXORPS:
        case I_VXORPD:
        case I_VORPS:
        case I_VORPD:
        case I_VANDPS:
        case I_VANDPD:
        case I_VANDNPS:
        case I_VANDNPD:
        case I_VPMINUB:
        case I_VPMINUD:
        case I_VPMAXUB:
        case I_VPMAXUD:
        case I_VPMOVMSKB:
        case I_VPSHUFB:
        case I_VBROADCASTSS:
        case I_VZEROUPPER:
        case I_VZEROALL:
            return DecodeAvx(insn);
        default:
            // Legacy SSE3 / SSSE3 / SSE4.1 / SSE4.2 (decoder_sse4.cc). Routed
            // from the final fallback rather than from sixty `case` labels: an
            // opcode any earlier family claims never reaches here, so this arm
            // cannot collide with a case added to an earlier dispatch family.
            // DecodeSse4 returns false for anything it does not claim, which
            // preserves the previous FALLBACK behaviour exactly.
            // The SSE4.2 string family (decoder_sse42str.cc) rides the same
            // arm for the same reason.
            return DecodeSse4(insn) || DecodeSse42Str(insn);
    }
    return true;
}

#undef SVM_DISPATCH_STAGE
#undef __

}  // namespace swift::x86
