#pragma once

#include "consumer_roles.h"
#include "guest_state.h"

namespace swift::runtime::compile {
// Mandatory sealed-IR summary. Existing Inst counters remain the source of
// exact use counts; this stores only the facts needed to skip empty families.
class BlockSummary final {
public:
    void Record(const ir::Inst& inst) {
        using O = ir::OpCode;
        const auto op = inst.GetOp();
        ++instruction_count;
        if (op >= O::LoadMemory && op <= O::StoreMemoryTSO) presence |= Memory;
        if (op == O::GetHostFPR || op == O::SetHostFPR || ir::IsFloatValueType(inst.ReturnType()))
            presence |= FPR;
        if (op == O::SetHostFPR) presence |= FPRWrites;
        if (op == O::SetHostFPR && inst.GetArg<ir::Imm>(2).Get() == 0) {
            const auto value = inst.GetArg<ir::Value>(0);
            if (value.Defined() && !ir::IsFloatValueType(value.Type()) &&
                ir::GetValueSizeByte(value.Type()) == 8)
                presence |= Low64FPRWrites;
        }
        if (inst.GetUses(false) != inst.GetUses()) presence |= PseudoConsumers;
        if (op == O::VecLoadConst) presence |= VectorConstants;
        if (op == O::PushRSB) presence |= ReturnAddressPush;
        if (op == O::CallLambda || op == O::Sse42Str) presence |= HostCallCandidates;
        if (op == O::SetLocation) presence |= LocationWrites;
        if (op == O::CallReturn || op == O::PopRSB) presence |= GuestCallOrPop;
        if (HasRole(ConsumerRoles(op), ConsumerRole::Entry)) presence |= Consumers;
        if (op == O::Select) presence |= BooleanSelects;
        if (op == O::Xor) presence |= ScalarCopies;
        if (op == O::ZeroExtend32 || op == O::ZeroExtend32To64 || op == O::And)
            presence |= NarrowBits;
        if (inst.HasValue() && inst.GetUses() == 1) presence |= SingleUseValue;
        // These are conservative opcode candidates. Type/offset and physical
        // qualifications remain in the GPR input builder, including failures.
        if (op == O::GetHostGPR || op == O::SetHostGPR || op == O::BitExtract ||
            op == O::ZeroExtend32To64)
            presence |= GPRInputs;
        if (op == O::GetHostGPR || op == O::SetHostGPR || IsGuestStateGPRBarrierCandidate(op))
            presence |= GPRWindows;
    }
    [[nodiscard]] u32 InstructionCount() const { return instruction_count; }
    [[nodiscard]] bool HasMemoryInstructions() const { return presence & Memory; }
    [[nodiscard]] bool HasFPRInstructions() const { return presence & FPR; }
    [[nodiscard]] bool HasFPRWrites() const { return presence & FPRWrites; }
    [[nodiscard]] bool HasLow64FPRWrites() const { return presence & Low64FPRWrites; }
    [[nodiscard]] bool HasPseudoConsumers() const { return presence & PseudoConsumers; }
    [[nodiscard]] bool HasVectorConstants() const { return presence & VectorConstants; }
    [[nodiscard]] bool HasConsumerDecisions() const { return presence & Consumers; }
    [[nodiscard]] bool HasBooleanSelects() const { return presence & BooleanSelects; }
    [[nodiscard]] bool HasSingleUseValue() const { return presence & SingleUseValue; }
    [[nodiscard]] bool HasGPRInputCandidates() const { return presence & GPRInputs; }
    [[nodiscard]] bool HasReturnAddressPush() const { return presence & ReturnAddressPush; }
    [[nodiscard]] bool HasHostCallCandidates() const { return presence & HostCallCandidates; }
    [[nodiscard]] bool HasLocationWrites() const { return presence & LocationWrites; }
    [[nodiscard]] bool HasGuestCallOrPop() const { return presence & GuestCallOrPop; }
    [[nodiscard]] bool HasGPRWindowCandidates() const { return presence & GPRWindows; }
    [[nodiscard]] bool HasScalarCopyCandidates() const { return presence & ScalarCopies; }
    [[nodiscard]] bool HasNarrowBitCandidates() const { return presence & NarrowBits; }

private:
    enum Presence : u32 {
        Memory = 1,
        FPR = 2,
        VectorConstants = 4,
        Consumers = 8,
        BooleanSelects = 16,
        SingleUseValue = 32,
        GPRInputs = 64,
        ReturnAddressPush = 128,
        Low64FPRWrites = 256,
        PseudoConsumers = 512,
        HostCallCandidates = 1024,
        LocationWrites = 2048,
        GuestCallOrPop = 4096,
        GPRWindows = 8192,
        FPRWrites = 16384,
        ScalarCopies = 32768,
        NarrowBits = 65536,
    };
    u32 instruction_count{}, presence{};
};
static_assert(sizeof(BlockSummary) == 8);
}  // namespace swift::runtime::compile
