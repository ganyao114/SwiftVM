#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

void A64Decoder::VisitCryptoAES(const Instruction* instr) {
    // VIXL's AES visitor does not validate the opcode or size field. All four
    // allocated encodings operate on exactly sixteen bytes.
    const u32 op = instr->GetInstructionBits() & 0xFFFFFC00u;
    constexpr u32 aese = 0x4E284800, aesd = 0x4E285800;
    constexpr u32 aesmc = 0x4E286800, aesimc = 0x4E287800;
    if (op != aese && op != aesd && op != aesmc && op != aesimc) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto source = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    auto zero = __ VecDup64(ImmValue(0, ir::ValueType::U64)).SetType(ir::ValueType::V128);
    ir::Value result;
    if (op == aese || op == aesd) {
        // ARM adds the key before substitution; shared AES IR follows x86 and
        // adds it afterward. Snapshot both operands before an aliased write.
        auto old = ReadVRegister(instr->GetRd(), ir::ValueType::V128);
        auto keyed = __ VecXor(old, source).SetType(ir::ValueType::V128);
        result = (op == aese ? __ VecAesEncLast(keyed, zero)
                             : __ VecAesDecLast(keyed, zero)).SetType(ir::ValueType::V128);
    } else if (op == aesmc) {
        // Inverse ShiftRows/SubBytes followed by the forward transforms cancel,
        // leaving MixColumns. This keeps AES-capable hosts on native vector IR.
        auto inverse = __ VecAesDecLast(source, zero).SetType(ir::ValueType::V128);
        result = __ VecAesEnc(inverse, zero).SetType(ir::ValueType::V128);
    } else {
        auto forward = __ VecAesEncLast(source, zero).SetType(ir::ValueType::V128);
        result = __ VecAesDec(forward, zero).SetType(ir::ValueType::V128);
    }
    WriteVRegister(instr->GetRd(), result);
}

#undef __
}  // namespace swift::arm64
