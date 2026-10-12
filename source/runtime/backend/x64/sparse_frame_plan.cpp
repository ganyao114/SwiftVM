#include "sparse_frame_plan.h"
#include <limits>

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }

bool ControlWithoutOwnHome(const ir::Inst* inst) {
    // Positive emitter inventory. Do not infer this from VOID: memory stores
    // and MemoryCopy use their own frame slots for addresses across helpers.
    if (inst->ReturnType() != T::VOID) return false;
    switch (inst->GetOp()) {
        case O::Nop: case O::UniformBarrier: case O::XchgBarrier:
        case O::BranchOnlyEdges: case O::PushRSB: case O::PopRSB:
        case O::CallReturn: case O::AdvancePC: case O::SetLocation:
        case O::Goto: case O::NotGoto: case O::BindLabel:
        case O::StoreUniform: case O::DefineLocal: case O::StoreLocal:
        case O::MemoryBarrierTSO: case O::CheckMemoryAlignment:
        case O::SetHostGPR: case O::SetHostFPR:
        case O::SaveFlags: case O::BranchOnlyFlags:
        case O::PublishFCmpFlags: case O::PublishSse42StrFlags:
        case O::ClearFlags: case O::SetCarry: case O::SetOverflow:
        case O::InvertCarry:
            return true;
        default: return false;
    }
}
bool RegisterOnlyOutput(const ir::Inst* inst) {
    // These scalar emitters publish through Write (or scalar Phi Write) and
    // have no unconditional own-home access. New allocator candidates do not
    // automatically gain this permission. Flags metadata is excluded outside.
    if (!Scalar(inst->ReturnType())) return false;
    switch (inst->GetOp()) {
        case O::LoadUniform:
            return inst->ArgAt(0).GetType() == ir::ArgType::Uniform &&
                   Scalar(inst->GetArg<ir::Uniform>(0).GetType());
        case O::AddPhi: case O::LoadImm: case O::GetOperand: case O::CallLambda:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::And: case O::Or: case O::Xor: case O::AndNot:
        case O::Neg: case O::Not: case O::Mul: case O::MulSub: case O::MulHigh:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
        case O::ByteSwap: case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
            return true;
        default: return false;
    }
}
struct HomeAuthority {
    std::unordered_set<const ir::Inst*> unused, live_definitions;
    explicit HomeAuthority(const CfgRegisterPlan& registers) {
        for (const auto& allocation : registers.Allocations()) {
            if (!allocation.uses) unused.insert(allocation.producer);
            else live_definitions.insert(allocation.producer);
        }
        std::unordered_set<const ir::Inst*> reachable;
        for (const auto& block : registers.Blocks()) if (block.reachable)
            for (u32 at = block.begin; at < block.end; ++at) reachable.insert(registers.Instructions().at(at));
        for (auto it = live_definitions.begin(); it != live_definitions.end();)
            if (!reachable.contains(*it)) it = live_definitions.erase(it); else ++it;
    }
};
std::optional<u64> RematerializableValue(const ir::Inst* inst, const CfgRegisterPlan& registers,
                                       const std::unordered_set<ir::Inst*>& flags,
                                       const HomeAuthority& authority, bool rematerialization) {
    if (!rematerialization || inst->GetOp() != O::LoadImm || !Scalar(inst->ReturnType()) ||
        inst->ArgAt(0).GetType() != ir::ArgType::Imm || registers.Register(inst) ||
        !authority.live_definitions.contains(inst) || registers.RequiresFullContainer(inst) ||
        flags.contains(const_cast<ir::Inst*>(inst))) return std::nullopt;
    for (u32 slot = 1; slot < ir::Inst::max_args; ++slot)
        if (!inst->ArgAt(slot).IsVoid()) return std::nullopt;
    // Match EmitLoadImm followed by Write: producer truncation zero-extends
    // the bit pattern for signed and unsigned returns alike.
    const u32 bits = ir::GetValueSizeByte(inst->ReturnType()) * 8;
    const u64 value = inst->GetArg<ir::Imm>(0).Get();
    return bits == 64 ? value : value & ((u64{1} << bits) - 1);
}
SparseHomeDecision Choose(const ir::Inst* inst, const backend::FunctionPlan* function,
                          const CfgRegisterPlan& registers,
                          const std::unordered_set<ir::Inst*>& flags, const HomeAuthority& authority,
                          bool rematerialization) {
    const bool marker = function && (function->exits.contains(const_cast<ir::Inst*>(inst)) ||
                                     function->halt_checks.contains(const_cast<ir::Inst*>(inst)));
    if (marker || ControlWithoutOwnHome(inst)) return SparseHomeDecision::Control;
    if (RematerializableValue(inst, registers, flags, authority, rematerialization))
        return SparseHomeDecision::Rematerialized;
    if (RegisterOnlyOutput(inst) && !registers.RequiresCanonicalHome(inst) &&
        !flags.contains(const_cast<ir::Inst*>(inst)) && (registers.Register(inst) || authority.unused.contains(inst)))
        return registers.Register(inst) ? SparseHomeDecision::RegisterOnly : SparseHomeDecision::UnusedScalar;
    return SparseHomeDecision::Retained;
}
bool PoolEligible(const ir::Inst* inst, const CfgRegisterPlan& registers,
                   const std::unordered_set<ir::Inst*>& flags, const HomeAuthority& authority) {
    return Scalar(inst->ReturnType()) && inst->GetOp() != O::AddPhi && RegisterOnlyOutput(inst) &&
           registers.RequiresCanonicalHome(inst) && !registers.RequiresFullContainer(inst) &&
           !flags.contains(const_cast<ir::Inst*>(inst)) && authority.live_definitions.contains(inst);
}
} // namespace

SparseFramePlan SparseFramePlan::Build(const ir::Block* body,
                                       const backend::FunctionPlan* function,
                                       const CfgRegisterPlan& registers,
                                       const std::unordered_set<ir::Inst*>& flag_producers,
                                       bool rematerialization) {
    SparseFramePlan result;
    if (!body || (function && function->body && function->body.get() != body) ||
        !registers.HasInterference() || !registers.Validate() || !registers.MatchesBody(body))
        return result;
    result.body = body;
    result.scalar_rematerialization_enabled = rematerialization;
    const HomeAuthority authority{registers};
    for (const auto& inst : body->GetInstList()) {
        const auto decision = Choose(&inst, function, registers, flag_producers, authority, rematerialization);
        if (decision == SparseHomeDecision::Control) {
            ++result.stats.omitted_control;
        } else if (decision == SparseHomeDecision::Rematerialized) {
            ++result.stats.omitted_rematerialized;
            result.rematerialized_values.emplace(&inst, *RematerializableValue(&inst, registers, flag_producers, authority, rematerialization));
        } else if (decision != SparseHomeDecision::Retained) {
            if (decision == SparseHomeDecision::RegisterOnly) {
                ++result.stats.omitted_register;
            } else {
                ++result.stats.omitted_unused;
            }
        } else {
            const bool pool = PoolEligible(&inst, registers, flag_producers, authority);
            HomeSlot* selected{};
            if (pool) for (auto& slot : result.slots) if (slot.scalar_pool) {
                bool conflict{};
                for (auto* owner : slot.owners)
                    conflict |= registers.Interferes(owner, &inst) || registers.Interferes(&inst, owner);
                if (!conflict) { selected = &slot; break; }
            }
            if (!selected) {
                if (result.stats.home_bytes > std::numeric_limits<u32>::max() - 32) return {};
                result.slots.push_back({result.stats.home_bytes, pool, {}});
                selected = &result.slots.back();
                result.stats.home_bytes += 32;
                ++result.stats.home_count;
            } else {
                ++result.stats.reused_scalar_homes;
                if (selected->owners.size() == 1) ++result.stats.shared_scalar_home_slots;
            }
            selected->owners.push_back(&inst);
            result.homes.emplace(&inst, selected->offset);
            ++result.stats.retained_homes;
        }
        result.decisions.emplace(&inst, decision);
    }
    result.enabled = true;
    if (!result.Validate(body, function, registers, flag_producers, rematerialization)) return {};
    return result;
}
bool SparseFramePlan::Validate(const ir::Block* candidate, const backend::FunctionPlan* function,
                                const CfgRegisterPlan& registers,
                                const std::unordered_set<ir::Inst*>& flag_producers,
                                bool rematerialization) const {
    if (!enabled || rematerialization != scalar_rematerialization_enabled || !candidate || candidate != body ||
        (function && function->body && function->body.get() != candidate) ||
        !registers.HasInterference() || !registers.Validate() || !registers.MatchesBody(candidate)) return false;
    const HomeAuthority authority{registers};
    SparseFrameStats counted;
    std::unordered_set<const ir::Inst*> retained, owners;
    size_t instructions{};
    for (const auto& inst : candidate->GetInstList()) {
        ++instructions;
        const auto found = decisions.find(&inst);
        if (found == decisions.end() || found->second != Choose(&inst, function, registers, flag_producers, authority, rematerialization)) return false;
        switch (found->second) {
            case SparseHomeDecision::Retained: retained.insert(&inst); ++counted.retained_homes; break;
            case SparseHomeDecision::Control: ++counted.omitted_control; break;
            case SparseHomeDecision::RegisterOnly: ++counted.omitted_register; break;
            case SparseHomeDecision::UnusedScalar: ++counted.omitted_unused; break;
            case SparseHomeDecision::Rematerialized: ++counted.omitted_rematerialized; break;
        }
        if ((found->second == SparseHomeDecision::Retained) != homes.contains(&inst)) return false;
        const auto constant = rematerialized_values.find(&inst);
        if ((found->second == SparseHomeDecision::Rematerialized) != (constant != rematerialized_values.end())) return false;
        if (constant != rematerialized_values.end() &&
            RematerializableValue(&inst, registers, flag_producers, authority, rematerialization) != constant->second) return false;
    }
    if (decisions.size() != instructions || homes.size() != retained.size() ||
        rematerialized_values.size() != counted.omitted_rematerialized) return false;
    for (const auto& slot : slots) {
        if (slot.offset != counted.home_bytes || slot.owners.empty() || (!slot.scalar_pool && slot.owners.size() != 1)) return false;
        if (counted.home_bytes > std::numeric_limits<u32>::max() - 32) return false;
        counted.home_bytes += 32; ++counted.home_count;
        if (slot.owners.size() >= 2) ++counted.shared_scalar_home_slots;
        for (size_t i = 0; i < slot.owners.size(); ++i) {
            const auto* owner = slot.owners[i];
            if (!retained.contains(owner) || !owners.insert(owner).second || homes.at(owner) != slot.offset ||
                slot.scalar_pool != PoolEligible(owner, registers, flag_producers, authority)) return false;
            for (size_t j = 0; j < i; ++j)
                if (registers.Interferes(owner, slot.owners[j]) || registers.Interferes(slot.owners[j], owner)) return false;
        }
    }
    counted.reused_scalar_homes = counted.retained_homes - counted.home_count;
    return owners.size() == retained.size() && counted.home_count == stats.home_count &&
           counted.retained_homes == stats.retained_homes && counted.home_bytes == stats.home_bytes &&
           counted.omitted_control == stats.omitted_control && counted.omitted_register == stats.omitted_register &&
           counted.omitted_unused == stats.omitted_unused && counted.reused_scalar_homes == stats.reused_scalar_homes &&
           counted.omitted_rematerialized == stats.omitted_rematerialized &&
           counted.shared_scalar_home_slots == stats.shared_scalar_home_slots;
}
std::optional<u32> SparseFramePlan::Home(const ir::Inst* inst) const noexcept {
    const auto found = homes.find(inst);
    return found == homes.end() ? std::nullopt : std::optional<u32>{found->second};
}
std::optional<u64> SparseFramePlan::RematerializedValue(const ir::Inst* inst) const noexcept {
    const auto found = rematerialized_values.find(inst);
    return found == rematerialized_values.end() ? std::nullopt : std::optional<u64>{found->second};
}
SparseHomeDecision SparseFramePlan::Decision(const ir::Inst* inst) const noexcept {
    const auto found = decisions.find(inst);
    return found == decisions.end() ? SparseHomeDecision::Retained : found->second;
}
} // namespace swift::runtime::backend::x64

