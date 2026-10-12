#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryBaseMemoryExtension(const Instruction* instr) {
    const u32 word=instr->GetInstructionBits();
    const bool unprivileged=(word&0x3B200C00u)==0x38000800u;
    const bool rcpc=(word&0x3B20FC00u)==0x3820C000u;
    if(!unprivileged&&!rcpc)return false;
    bool load=false,sign=false,destination64=false;
    u32 bytes=0;
    if(rcpc){
        // LDAPR has no source register. Rs must be exactly 31; no LSE RMW
        // mode or release variant may be silently accepted by this helper.
        if(instr->GetRs()!=31){Interrupt(InterruptReason::ILL_CODE,current_pc);return true;}
        switch(instr->Mask(AtomicMemoryMask)){
            case LDAPRB:bytes=1;break;
            case LDAPRH:bytes=2;break;
            case LDAPR_w:bytes=4;break;
            case LDAPR_x:bytes=8;destination64=true;break;
            default:Interrupt(InterruptReason::ILL_CODE,current_pc);return true;
        }
        load=true;
    }else{
        // VIXL routes this whole class to VisitUnimplemented, so validate the
        // SIMD bit and all opc/size combinations here, including reserved ones.
        if(word&(1u<<26)){Interrupt(InterruptReason::ILL_CODE,current_pc);return true;}
        switch(instr->Mask(LoadStoreMask)){
            case STRB_w:bytes=1;break;
            case STRH_w:bytes=2;break;
            case STR_w:bytes=4;break;
            case STR_x:bytes=8;break;
            case LDRB_w:bytes=1;load=true;break;
            case LDRH_w:bytes=2;load=true;break;
            case LDR_w:bytes=4;load=true;break;
            case LDR_x:bytes=8;load=destination64=true;break;
            case LDRSB_w:bytes=1;load=sign=true;break;
            case LDRSH_w:bytes=2;load=sign=true;break;
            case LDRSB_x:bytes=1;load=sign=destination64=true;break;
            case LDRSH_x:bytes=2;load=sign=destination64=true;break;
            case LDRSW_x:bytes=4;load=sign=destination64=true;break;
            default:Interrupt(InterruptReason::ILL_CODE,current_pc);return true;
        }
    }
    // User-mode Normal memory: LDTR uses the same EL0 permissions as ordinary
    // loads, and RCpc follows the existing nAA=1 unaligned-memory profile.
    // Neither instruction writes the base or performs an exclusive/RMW access.
    __ SetLocation(ir::Lambda{ir::Imm{current_pc}});
    auto base=ReadXRegister(instr->GetRn(),Reg31IsStackPointer);
    auto address=unprivileged?AddressAdd(base,s64(instr->GetImmLS())):base;
    const auto type=bytes==1?ir::ValueType::U8:bytes==2?ir::ValueType::U16:bytes==4?ir::ValueType::U32:ir::ValueType::U64;
    if(load){
        auto value=rcpc?__ LoadMemoryTSO(SingleOperand(address)).SetType(type):ReadMemory(ir::Lambda{address},type);
        if(sign)value=SignExtendValue(value,bytes*8);
        if(destination64)WriteXRegister(instr->GetRt(),value);else WriteWRegister(instr->GetRt(),value);
    }else WriteMemory(address,ReadRegister(instr->GetRt(),type));
    return true;
}

#undef __
}  // namespace swift::arm64
