#include "carry_chain_plan.h"
#include <limits>
#include "runtime/backend/context.h"

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
bool EmptyTail(const ir::Inst* inst, u32 begin) {
    for (u32 slot = begin; slot < ir::Inst::max_args; ++slot)
        if (!inst->ArgAt(slot).IsVoid()) return false;
    return true;
}
bool ScalarValue(const ir::Inst* inst, u32 slot) {
    return inst->ArgAt(slot).IsValue() && inst->GetArg<ir::Value>(slot).Defined() &&
           Scalar(inst->GetArg<ir::Value>(slot).Type());
}
bool Producer(const ir::Inst* inst, bool second) {
    const auto op = inst->GetOp();
    if (op != O::Adc && op != O::Sbb && (second || (op != O::Add && op != O::Sub))) return false;
    if (!Scalar(inst->ReturnType()) || !ScalarValue(inst, 0) ||
        !inst->ArgAt(1).IsOperand() || !EmptyTail(inst, 4)) return false;
    // Operands occupy physical slots 1..3. Reject even unused modifiers;
    // accepted reconstruction is one scalar MOV/Read with no ALU clobber.
    const auto rhs = inst->GetArg<ir::Operand>(1);
    const auto left = rhs.GetLeft();
    const auto modifier = rhs.GetOp();
    return rhs.GetRight().Null() && modifier.shift_ext == 0 &&
           (modifier.type == ir::OperandOp::None || modifier.type == ir::OperandOp::Plus) &&
           ((left.IsValue() && left.value.Defined() && Scalar(left.value.Type())) ||
            (left.IsImm() && Scalar(left.imm.GetType())));
}
bool FullPublication(const ir::Inst* marker, const ir::Inst* producer) {
    return marker->GetOp() == O::SaveFlags && marker->ReturnType() == T::VOID &&
           ScalarValue(marker, 0) && marker->GetArg<ir::Value>(0).Def() == producer &&
           marker->ArgAt(1).GetType() == ir::ArgType::Flags &&
           marker->GetArg<ir::Flags>(1) == ir::Flags::NZCV && EmptyTail(marker, 2);
}
bool UniformRange(u32 offset, u32 size, std::optional<u64> extent) {
    // A known extent confines MOVs to State's uniform buffer. This excludes
    // wraparound into packed flags/PC/recovery state and invalid host accesses.
    return extent && size && size <= 8 && offset <= *extent && size <= *extent - offset &&
           u64{state_offset_uniform_buffer} + offset + size - 1 <= u64{std::numeric_limits<s32>::max()};
}
bool PreservesCarry(const ir::Inst* inst, std::optional<u64> extent) {
    switch (inst->GetOp()) {
        case O::Nop:
            return inst->ReturnType() == T::VOID && EmptyTail(inst, 0);
        case O::AdvancePC:
            // EmitAdvancePC is compile-time bookkeeping only: no native
            // instruction, poll, flags/PC publication or recovery boundary.
            return inst->ReturnType() == T::VOID && inst->ArgAt(0).IsImm() &&
                   Scalar(inst->GetArg<ir::Imm>(0).GetType()) && EmptyTail(inst, 1);
        case O::ZeroExtend64:
            // A64 WriteWRegister's publication normalization: audited scalar
            // ReadUnaryInput/Mask/Write paths emit MOV/MOVZX exclusively.
            return inst->ReturnType() == T::U64 && ScalarValue(inst, 0) && EmptyTail(inst, 1);
        case O::LoadImm:
            // EmitLoadImm: MOV + Write (or no emission if rematerialized).
            return Scalar(inst->ReturnType()) && inst->ArgAt(0).IsImm() &&
                   Scalar(inst->GetArg<ir::Imm>(0).GetType()) && EmptyTail(inst, 1);
        case O::LoadUniform: {
            if (!Scalar(inst->ReturnType()) || inst->ArgAt(0).GetType() != ir::ArgType::Uniform ||
                !EmptyTail(inst, 1)) return false;
            const auto uniform = inst->GetArg<ir::Uniform>(0);
            return Scalar(uniform.GetType()) && UniformRange(uniform.GetOffset(),
                ir::GetValueSizeByte(uniform.GetType()), extent);
        }
        case O::StoreUniform: {
            if (inst->ReturnType() != T::VOID || inst->ArgAt(0).GetType() != ir::ArgType::Uniform ||
                !ScalarValue(inst, 1) || !EmptyTail(inst, 2)) return false;
            const auto uniform = inst->GetArg<ir::Uniform>(0);
            // StoreUniform follows the value view, not the declared uniform
            // width. Read/Mask/publication are MOV/MOVZX only in this scope.
            return Scalar(uniform.GetType()) && UniformRange(uniform.GetOffset(),
                ir::GetValueSizeByte(inst->GetArg<ir::Value>(1).Type()), extent);
        }
        default: return false;
    }
}
bool Boundary(ir::Inst* inst, const backend::FunctionPlan* function) {
    return function && (function->exits.contains(inst) ||
        function->halt_checks.contains(inst) || function->stops.contains(inst));
}
bool Subtract(const ir::Inst* inst) { return inst->GetOp() == O::Sub || inst->GetOp() == O::Sbb; }
} // namespace

CarryChainPlan CarryChainPlan::Build(const CfgRegisterPlan& graph,
                                    const backend::FunctionPlan* function,
                                    std::optional<u64> extent) {
    CarryChainPlan result;
    if (!graph.Enabled() || !graph.MatchesBody(graph.Body()) ||
        (function && function->body && function->body.get() != graph.Body())) return result;
    const auto& instructions = graph.Instructions();
    std::unordered_set<const ir::Inst*> injected_phi_boundaries;
    for (const auto& edge : graph.Edges())
        if (edge.source && !edge.taken && !edge.copies.empty()) injected_phi_boundaries.insert(edge.source);
    const auto boundary = [&](ir::Inst* inst) {
        // Translate emits fallthrough Phi transport before this source IR.
        // Keep that injected sequence outside the positive emitter proof.
        return Boundary(inst, function) || injected_phi_boundaries.contains(inst);
    };
    for (const auto& block : graph.Blocks()) {
        if (!block.reachable) continue;
        for (u32 begin = block.begin; begin < block.end; ++begin) {
            auto* first = instructions[begin];
            if (boundary(first) || !Producer(first, false)) continue;
            u32 cursor = begin + 1;
            const auto skip_moves = [&]() {
                while (cursor < block.end && !boundary(instructions[cursor]) &&
                       PreservesCarry(instructions[cursor], extent)) ++cursor;
            };
            skip_moves();
            if (cursor == block.end || boundary(instructions[cursor]) ||
                !FullPublication(instructions[cursor], first)) continue;
            const auto* first_publication = instructions[cursor++];
            skip_moves();
            if (cursor == block.end || boundary(instructions[cursor]) ||
                !Producer(instructions[cursor], true)) continue;
            const auto* second = instructions[cursor++];
            if (second->ReturnType() != first->ReturnType()) continue;
            skip_moves();
            if (cursor == block.end || boundary(instructions[cursor]) ||
                !FullPublication(instructions[cursor], second)) continue;
            result.incoming.emplace(second, Incoming{first, Subtract(first) != Subtract(second)});
            result.deferred_publications.insert(first_publication);
            // Disjoint pairs only: never defer a publication which finishes
            // an earlier pair. The final full-NZCV merge publishes both.
            begin = cursor;
        }
    }
    return result;
}
const CarryChainPlan::Incoming* CarryChainPlan::IncomingFor(const ir::Inst* second) const noexcept {
    const auto found = incoming.find(second);
    return found == incoming.end() ? nullptr : &found->second;
}
} // namespace swift::runtime::backend::x64
