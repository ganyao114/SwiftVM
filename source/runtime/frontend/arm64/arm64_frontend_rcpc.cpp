#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

void A64Decoder::VisitLoadStoreRCpcUnscaledOffset(const Instruction* instr) {
    // User-mode Normal-memory profile: SCTLR.nAA=1 permits accesses crossing
    // a 16-byte boundary. This frontend does not model privileged alignment
    // controls or Device memory. Do not infer this policy from VIXL's simulator,
    // which instead rejects crossings, or from single-threaded oracle results.
    const auto op = instr->Mask(LoadStoreRCpcUnscaledOffsetMask);
    bool load{}, sign{}, destination64{};
    u32 bits{};
    switch (op) {
        case STLURB: bits = 8; break;
        case STLURH: bits = 16; break;
        case STLUR_w: bits = 32; break;
        case STLUR_x: bits = 64; break;
        case LDAPURB: bits = 8; load = true; break;
        case LDAPURH: bits = 16; load = true; break;
        case LDAPUR_w: bits = 32; load = true; break;
        case LDAPUR_x: bits = 64; load = destination64 = true; break;
        case LDAPURSB_w: bits = 8; load = sign = true; break;
        case LDAPURSB_x: bits = 8; load = sign = destination64 = true; break;
        case LDAPURSH_w: bits = 16; load = sign = true; break;
        case LDAPURSH_x: bits = 16; load = sign = destination64 = true; break;
        case LDAPURSW: bits = 32; load = sign = destination64 = true; break;
        default: Interrupt(InterruptReason::ILL_CODE, current_pc); return;
    }
    __ SetLocation(ir::Lambda{ir::Imm{current_pc}});
    auto base = ReadXRegister(instr->GetRn(), Reg31IsStackPointer);
    auto address = AddressAdd(base, s64(instr->GetImmLS()));
    const auto type = bits == 8 ? ir::ValueType::U8 : bits == 16 ? ir::ValueType::U16 :
                      bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64;
    if (load) {
        auto value = __ LoadMemoryTSO(SingleOperand(address)).SetType(type);
        if (sign) value = SignExtendValue(value, bits);
        if (destination64) WriteXRegister(instr->GetRt(), value);
        else WriteWRegister(instr->GetRt(), value);
    } else {
        __ StoreMemoryTSO(SingleOperand(address), ReadRegister(instr->GetRt(), type));
    }
}

#undef __
}  // namespace swift::arm64
