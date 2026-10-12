#include "arm64_frontend_internal.h"
#include "single_instruction_admission.h"
#include "fp_helpers.h"
#include "runtime/backend/context.h"
#include <cstring>

namespace swift::arm64 {
namespace {
enum class IndexedOperation { Multiply, ExtendedMultiply, MultiplyAdd, MultiplySubtract };
struct IndexedFP {
    u32 bits, rd, rn, rm, index, active;
    IndexedOperation operation;
    bool allocated;
};

bool DecodeIndexedFP(u32 word, IndexedFP& decoded) {
    const bool scalar = word & NEONScalar;
    const u32 mask = scalar ? u32(NEONScalarByIndexedElementMask) : u32(NEONByIndexedElementMask);
    const u32 flags = scalar ? u32(NEON_Q | NEONScalar) : 0;
    const u32 op = word & mask;
    if (op == (u32(NEON_FMUL_H_byelement) | flags)) decoded.operation = IndexedOperation::Multiply;
    else if (op == (u32(NEON_FMULX_H_byelement) | flags)) decoded.operation = IndexedOperation::ExtendedMultiply;
    else if (op == (u32(NEON_FMLA_H_byelement) | flags)) decoded.operation = IndexedOperation::MultiplyAdd;
    else if (op == (u32(NEON_FMLS_H_byelement) | flags)) decoded.operation = IndexedOperation::MultiplySubtract;
    else return false;
    const u32 size = (word >> 22) & 3, h = (word >> 11) & 1, l = (word >> 21) & 1, m = (word >> 20) & 1;
    decoded.bits = size == 0 ? 16 : size == 2 ? 32 : 64;
    decoded.rd = word & 31; decoded.rn = (word >> 5) & 31;
    // H uses H:L:M as its index and only four Rm bits. S/D use M as Rm[4].
    decoded.rm = (word >> 16) & (decoded.bits == 16 ? 15 : 31);
    decoded.index = decoded.bits == 16 ? (h << 2) | (l << 1) | m :
                    decoded.bits == 32 ? (h << 1) | l : h;
    decoded.active = scalar ? 1 : (word & NEON_Q ? 128 : 64) / decoded.bits;
    decoded.allocated = size != 1 && (decoded.bits != 64 || (!l && (scalar || (word & NEON_Q))));
    return true;
}

u64 ExecuteIndexedFP(u64 context_address, u64 instruction, u64 guest_pc) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context_address);
    IndexedFP decoded;
    if (!IsSupportedFPCR(ctx.fpcr) || !DecodeIndexedFP(u32(instruction), decoded) || !decoded.allocated) {
        auto& state = *reinterpret_cast<runtime::backend::State*>(
            context_address - runtime::backend::state_offset_uniform_buffer);
        ctx.pc = guest_pc;
        ctx.padding = u32(InterruptReason::FALLBACK);
        state.current_loc = runtime::ir::Location{guest_pc};
        state.halt_reason = runtime::HaltReason::CallHost;
        return 0;
    }
    u8 lhs[16], indexed[16], accumulator[16], destination[16]{};
    std::memcpy(lhs, &ctx.v[decoded.rn], sizeof(lhs));
    std::memcpy(indexed, &ctx.v[decoded.rm], sizeof(indexed));
    std::memcpy(accumulator, &ctx.v[decoded.rd], sizeof(accumulator));
    u64 rhs = 0;
    std::memcpy(&rhs, indexed + decoded.index * decoded.bits / 8, decoded.bits / 8);
    u32 exceptions = 0;
    for (u32 lane = 0; lane < decoded.active; ++lane) {
        u64 left = 0, addend = 0;
        std::memcpy(&left, lhs + lane * decoded.bits / 8, decoded.bits / 8);
        std::memcpy(&addend, accumulator + lane * decoded.bits / 8, decoded.bits / 8);
        FPBinaryResult result;
        switch (decoded.operation) {
            case IndexedOperation::Multiply: result = EvaluateBinaryFP(left, rhs, decoded.bits, 0, ctx.fpcr); break;
            case IndexedOperation::ExtendedMultiply: result = EvaluateExtendedMultiplyFP(left, rhs, decoded.bits, ctx.fpcr); break;
            default: result = EvaluateFusedFP(left, rhs, addend, decoded.bits,
                decoded.operation == IndexedOperation::MultiplySubtract ? 1 : 0, ctx.fpcr); break;
        }
        std::memcpy(destination + lane * decoded.bits / 8, &result.value, decoded.bits / 8);
        exceptions |= result.exceptions;
    }
    ctx.fpsr |= exceptions;
    std::memcpy(&ctx.v[decoded.rd], destination, sizeof(destination));
    return 0;
}
}

bool A64Decoder::TryNEONIndexedFP(const Instruction* instr) {
    IndexedFP decoded;
    if (!DecodeIndexedFP(instr->GetInstructionBits(), decoded)) return false;
    if (!decoded.allocated) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto context = assembler->GetUniformAddress(ir::Imm{u32{0}}).SetType(ir::ValueType::U64);
    assembler->CallHostFPFree(&ExecuteIndexedFP, context,
        ImmValue(instr->GetInstructionBits(), ir::ValueType::U64),
        ImmValue(current_pc, ir::ValueType::U64)).SetType(ir::ValueType::U64);
    return true;
}
bool detail::AdmitIndexedFP(u64 target, u32 word) noexcept {
    IndexedFP decoded;
    return SingleInstructionTarget(target, &ExecuteIndexedFP) && DecodeIndexedFP(word, decoded) && decoded.allocated;
}

}  // namespace swift::arm64
