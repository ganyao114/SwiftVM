#include "arm64_frontend_internal.h"

namespace swift::arm64 {

#define __ assembler->

namespace {
constexpr auto DCZID_EL0 = SystemRegisterEncoder<3, 3, 0, 0, 7>::value;
}  // namespace

void A64Decoder::VisitPCRelAddressing(const Instruction* instr) {
    VAddr address;
    if (instr->Mask(PCRelAddressingMask) == ADRP) {
        address = (current_pc & ~VAddr(0xFFF)) + s64(instr->GetImmPCRel()) * s64(kPageSize);
    } else {
        VIXL_ASSERT(instr->Mask(PCRelAddressingMask) == ADR);
        address = current_pc + s64(instr->GetImmPCRel());
    }
    WriteXRegister(instr->GetRd(),
                   __ LoadImm<ir::U64>(ir::Imm{address}).SetType(ir::ValueType::U64));
}

void A64Decoder::VisitUnconditionalBranch(const Instruction* instr) {
    VAddr target = current_pc + s64(instr->GetImmUncondBranch()) * kInstructionSize;
    if (instr->Mask(UnconditionalBranchMask) == BL) {
        WriteXRegister(kLinkRegCode,
                       __ LoadImm<ir::U64>(ir::Imm{NextPC()}).SetType(ir::ValueType::U64));
        __ PushRSB(ir::Lambda{ir::Imm{NextPC()}});
        if (assembler->IsFunctionMode()) {
            // A direct call ends this compilation region. Treating BL like an
            // intra-function branch would absorb the callee (and recursive
            // callees) into the caller CFG, while its RET must resume at a
            // separately dispatchable return address.
            WritePC(ir::Lambda{ir::Imm{target}});
            __ ReturnToDispatcher();
            end_decode_ = true;
            return;
        }
    }
    BranchImm(target);
}

void A64Decoder::VisitConditionalBranch(const Instruction* instr) {
    VIXL_ASSERT(instr->Mask(ConditionalBranchMask) == B_cond);
    VAddr target = current_pc + s64(instr->GetImmCondBranch()) * kInstructionSize;
    auto cond = CondPassed(static_cast<Condition>(instr->GetConditionBranch()));
    assembler->If(ir::terminal::If{
            cond, ir::terminal::LinkBlock{target}, ir::terminal::LinkBlock{NextPC()}});
    end_decode_ = true;
}

void A64Decoder::VisitCompareBranch(const Instruction* instr) {
    bool is64 = false;
    bool non_zero = false;
    switch (instr->Mask(CompareBranchMask)) {
        case CBZ_w:
            break;
        case CBZ_x:
            is64 = true;
            break;
        case CBNZ_w:
            non_zero = true;
            break;
        case CBNZ_x:
            is64 = true;
            non_zero = true;
            break;
        default:
            VIXL_UNREACHABLE();
    }
    auto value = ReadRegister(instr->GetRt(), GPRType(is64));
    ir::BOOL cond = non_zero ? __ TestNotZero(value) : __ TestZero(value);
    VAddr target = current_pc + s64(instr->GetImmCmpBranch()) * kInstructionSize;
    assembler->If(ir::terminal::If{
            cond, ir::terminal::LinkBlock{target}, ir::terminal::LinkBlock{NextPC()}});
    end_decode_ = true;
}

void A64Decoder::VisitTestBranch(const Instruction* instr) {
    u32 bit_pos = (instr->GetImmTestBranchBit5() << 5) | instr->GetImmTestBranchBit40();
    auto value = ReadXRegister(instr->GetRt());
    ir::BOOL bit_set = __ TestBit(value, ir::Imm{u8(bit_pos)});
    ir::BOOL cond = bit_set;
    if (instr->Mask(TestBranchMask) == TBZ) {
        cond = ir::BOOL{__ Xor<ir::U8>(bit_set, SingleOperand(ir::Imm{true}))};
    }
    VAddr target = current_pc + s64(instr->GetImmTestBranch()) * kInstructionSize;
    assembler->If(ir::terminal::If{
            cond, ir::terminal::LinkBlock{target}, ir::terminal::LinkBlock{NextPC()}});
    end_decode_ = true;
}

void A64Decoder::VisitUnconditionalBranchToRegister(const Instruction* instr) {
    switch (instr->Mask(UnconditionalBranchToRegisterMask)) {
        case RET: {
            WritePC(ir::Lambda{ReadXRegister(instr->GetRn())});
            // NOTE: PopRSB is not emitted: zero-argument IR instructions
            // currently crash Inst::Validate (IR issue, see report).
            __ Return();
            end_decode_ = true;
            break;
        }
        case BR: {
            WritePC(ir::Lambda{ReadXRegister(instr->GetRn())});
            __ ReturnToDispatcher();
            end_decode_ = true;
            break;
        }
        case BLR: {
            auto target = ReadXRegister(instr->GetRn());
            WriteXRegister(kLinkRegCode,
                       __ LoadImm<ir::U64>(ir::Imm{NextPC()}).SetType(ir::ValueType::U64));
            __ PushRSB(ir::Lambda{ir::Imm{NextPC()}});
            WritePC(ir::Lambda{target});
            __ ReturnToDispatcher();
            end_decode_ = true;
            break;
        }
        default:
            // Pointer authentication branches are not supported yet.
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
    }
}

void A64Decoder::VisitSystem(const Instruction* instr) {
    if (instr->Mask(SystemHintFMask) == SystemHintFixed) {
        // NOP, YIELD, WFE, WFI, SEV(L), BTI, ... are all treated as NOPs.
        // NOTE: nothing is emitted: the zero-argument Nop IR instruction
        // currently crashes Inst::Validate (IR issue, see report).
        return;
    }

    if (instr->Mask(MemBarrierFMask) == MemBarrierFixed) {
        // DMB / DSB / ISB: no reordering is performed by this IR, so they are
        // dropped here as well.
        return;
    }

    if (instr->Mask(SystemSysRegFMask) == SystemSysRegFixed) {
        auto sysreg = instr->GetImmSystemRegister();
        bool is_read = instr->Mask(SystemSysRegMask) == MRS;
        auto rt = instr->GetRt();
        switch (sysreg) {
            case TPIDR_EL0:
            case TPIDRRO_EL0: {
                ir::Uniform uni{u32(offsetof(ThreadContext64, tpidr)), ir::ValueType::U64};
                if (is_read) {
                    WriteXRegister(rt, __ LoadUniform<ir::U64>(uni));
                } else {
                    __ StoreUniform(uni, ReadXRegister(rt));
                }
                return;
            }
            case FPCR: {
                ir::Uniform uni{u32(offsetof(ThreadContext64, fpcr)), ir::ValueType::U32};
                if (is_read) {
                    WriteWRegister(rt, __ LoadUniform<ir::U32>(uni));
                } else {
                    __ StoreUniform(uni, ReadRegister(rt, ir::ValueType::U32));
                }
                return;
            }
            case FPSR: {
                ir::Uniform uni{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
                if (is_read) {
                    WriteWRegister(rt, __ LoadUniform<ir::U32>(uni));
                } else {
                    __ StoreUniform(uni, ReadRegister(rt, ir::ValueType::U32));
                }
                return;
            }
            case DCZID_EL0:
                if (is_read) {
                    // DZP=1: DC ZVA is prohibited. Returning zero would
                    // advertise a supported four-byte zeroing block.
                    WriteXRegister(rt, ImmValue(1u << 4, ir::ValueType::U64));
                }
                return;
            default:
                // Unknown system registers (NZCV, MIDR_EL1, …): MRS reads
                // return 0; MSR writes are ignored.
                if (is_read) {
                    WriteXRegister(rt, ImmValue(0, ir::ValueType::U64));
                }
                return;
        }
    }

    Interrupt(InterruptReason::FALLBACK, current_pc);
}

void A64Decoder::VisitException(const Instruction* instr) {
    switch (instr->Mask(ExceptionMask)) {
        case SVC:
            // System call: hand over to the host, resuming at the next
            // instruction once the syscall has been serviced. The syscall
            // number / arguments are read from the guest context (x8, x0-x5).
            Interrupt(InterruptReason::SVC, NextPC());
            break;
        case BRK:
            Interrupt(InterruptReason::BRK, current_pc);
            break;
        case HLT:
            Interrupt(InterruptReason::HLT, current_pc);
            break;
        default:
            Interrupt(InterruptReason::FALLBACK, current_pc);
            break;
    }
}

}  // namespace swift::arm64
