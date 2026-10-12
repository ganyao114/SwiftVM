#include "flags_liveness.h"

#include <limits>
#include <unordered_map>
#include <vector>
#include "runtime/backend/context.h"

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using F = ir::Flags;
using Positions = std::unordered_map<ir::Inst*, u32>;

bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
u32 Width(T type) {
    return type >= T::U8 && type <= T::V128 ? ir::GetValueSizeByte(type) : 0;
}
bool PriorValue(ir::Value value, u32 position, const Positions& positions) {
    const auto found = positions.find(value.Def());
    // Type() can dereference the definition. Check membership and ordering
    // first, including for cast views of foreign/undefined definitions.
    return found != positions.end() && found->second < position &&
           Width(value.Def()->ReturnType()) && Width(value.Type());
}
bool ArithmeticProducer(ir::Inst* inst) {
    if (!Scalar(inst->ReturnType())) return false;
    switch (inst->GetOp()) {
        case O::Add: case O::Sub: case O::Adc: case O::Sbb: case O::Neg:
            return true;
        default: return false;
    }
}
bool FullPublication(ir::Inst* inst, u32 position, const Positions& positions) {
    if (inst->GetOp() != O::SaveFlags || !inst->ArgAt(0).IsValue() ||
        inst->ArgAt(1).GetType() != ir::ArgType::Flags ||
        inst->GetArg<F>(1) != F::NZCV) return false;
    for (u32 slot = 2; slot < ir::Inst::max_args; ++slot)
        if (!inst->ArgAt(slot).IsVoid()) return false;
    const auto value = inst->GetArg<ir::Value>(0);
    return PriorValue(value, position, positions) && Scalar(value.Type()) &&
           ArithmeticProducer(value.Def());
}
bool UniformAccess(ir::Inst* inst, u32 position, const Positions& positions, u32 extent) {
    if (inst->ArgAt(0).GetType() != ir::ArgType::Uniform) return false;
    const auto uniform = inst->GetArg<ir::Uniform>(0);
    const u32 first_unused = inst->GetOp() == O::LoadUniform ? 1 : 2;
    for (u32 slot = first_unused; slot < ir::Inst::max_args; ++slot)
        if (!inst->ArgAt(slot).IsVoid()) return false;
    u32 size{};
    if (inst->GetOp() == O::LoadUniform) {
        // The load uses its descriptor, even if its SSA annotation differs.
        if (!Width(inst->ReturnType())) return false;
        size = Width(uniform.GetType());
    } else {
        if (!inst->ArgAt(1).IsValue()) return false;
        const auto value = inst->GetArg<ir::Value>(1);
        if (!PriorValue(value, position, positions)) return false;
        // The store's input view owns the transfer width, not its descriptor.
        size = Width(value.Type());
    }
    const u32 offset = uniform.GetOffset();
    return size && offset <= extent && size <= extent - offset &&
           u64(state_offset_uniform_buffer) + offset + size <=
               u64(std::numeric_limits<s32>::max());
}
bool ScalarArguments(ir::Inst* inst, u32 position, const Positions& positions) {
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        auto& arg = inst->ArgAt(slot);
        if (arg.IsValue()) {
            const auto value = arg.Get<ir::Value>();
            if (!PriorValue(value, position, positions) || !Scalar(value.Type())) return false;
        } else {
            switch (arg.GetType()) {
                case ir::ArgType::Void: case ir::ArgType::Imm: case ir::ArgType::Operand:
                    break;
                default: return false;
            }
        }
    }
    return true;
}
bool Transparent(ir::Inst* inst, u32 position, const Positions& positions, u32 extent) {
    switch (inst->GetOp()) {
        case O::LoadUniform: case O::StoreUniform:
            return UniformAccess(inst, position, positions, extent);
        case O::Nop: case O::AdvancePC:
            return ScalarArguments(inst, position, positions);
        // Each audited lowering uses SSA scratch/home storage only. Native
        // arithmetic flags are transient; these opcodes do not publish guest
        // NZCV, call helpers, poll, or access guest memory. Division, PopCount,
        // raw/vector aliases and all unknown opcodes deliberately stay out.
        case O::LoadImm: case O::GetOperand:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::And: case O::Or: case O::Xor: case O::AndNot:
        case O::Neg: case O::Not: case O::Mul: case O::MulSub: case O::MulHigh:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
        case O::ByteSwap: case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::CountLeadingZeros32: case O::CountLeadingZeros64:
        case O::CountTrailingZeros32: case O::CountTrailingZeros64:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return Scalar(inst->ReturnType()) && ScalarArguments(inst, position, positions);
        default: return false;
    }
}
} // namespace

FlagsLivenessStats NarrowCarryChainFlags(ir::Block* block, u32 uniform_extent) {
    FlagsLivenessStats stats;
    if (!block) return stats;
    std::vector<ir::Inst*> instructions;
    Positions positions;
    for (auto& inst : block->GetInstList()) {
        switch (inst.GetOp()) {
            case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
                ++stats.barriers;
                return stats; // Refuse the entire block before any mutation.
            default: break;
        }
        positions.emplace(&inst, static_cast<u32>(instructions.size()));
        instructions.push_back(&inst);
    }
    F live = F::NZCV; // All flags are observable at every terminal/unit exit.
    for (auto next = instructions.rbegin(); next != instructions.rend(); ++next) {
        auto* inst = *next;
        const u32 position = positions.at(inst);
        if (FullPublication(inst, position, positions)) {
            ++stats.full_publications;
            if ((live & F::NZCV) == F::Carry) {
                inst->SetArg(1, F::Carry);
                ++stats.narrowed_to_carry;
            }
            // Kill the ORIGINAL exact publication, even when it was narrowed.
            // ADC/SBB's incoming C is read at its producer, not at SaveFlags.
            live = F::None;
        } else if (Transparent(inst, position, positions, uniform_extent)) {
            if (inst->GetOp() == O::Adc || inst->GetOp() == O::Sbb) live |= F::Carry;
        } else {
            // Partial/non-arithmetic publications, BranchOnlyFlags, all guest
            // flag consumers and helper/fault/opaque operations are barriers.
            live = F::NZCV;
            ++stats.barriers;
        }
    }
    return stats;
}
} // namespace swift::runtime::backend::x64
