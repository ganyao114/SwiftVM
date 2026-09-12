#include "translator.h"

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

#include <unordered_set>

namespace swift::runtime::backend::arm64 {

#define __ masm.

bool JitTranslator::RegionSuccessorAcceptsEdgeFlags(
        ir::Location target,
        const EdgeFlagsState& incoming) const {
    std::unordered_set<u64> visited;
    while (visited.insert(target.Value()).second) {
        const auto found = region_block_map.find(target.Value());
        auto* block = found == region_block_map.end() ? nullptr : found->second;
        if (!block) {
            return false;
        }
        HostFlags needed = static_cast<HostFlags>(incoming.valid_nzcv_mask);
        for (auto& inst : block->GetInstList()) {
            const auto op = inst.GetOp();
            if (op == ir::OpCode::GetFlags || op == ir::OpCode::CallLambda ||
                op == ir::OpCode::CallLocation || op == ir::OpCode::CallDynamic ||
                op == ir::OpCode::X87Op || op == ir::OpCode::TestFlags ||
                op == ir::OpCode::TestNotFlags || op == ir::OpCode::Adc ||
                op == ir::OpCode::Sbb || op == ir::OpCode::CondSelect ||
                op == ir::OpCode::CondSet) {
                return false;
            }
            if (op == ir::OpCode::ClearFlags) {
                needed &= static_cast<HostFlags>(
                        ~static_cast<u64>(GuestNZCVToHost(
                                inst.GetArg<ir::Flags>(0) & ir::Flags::NZCV)));
            } else if (op == ir::OpCode::BranchOnlyFlags) {
                return true;
            } else if (op == ir::OpCode::SaveFlags) {
                needed &= static_cast<HostFlags>(
                        ~static_cast<u64>(GuestNZCVToHost(
                                inst.GetArg<ir::Flags>(1))));
            }
            if (!True(needed)) {
                return true;
            }
        }
        // A flag-preserving block does not commit incoming NZCV on its own.
        // Follow its jump until the pending bits are overwritten; an observer,
        // external edge or cycle requires the predecessor to commit them.
        if (!BlockIsFlagsTransparent(block)) {
            return false;
        }
        const auto next = RegionLeafTarget(block->GetTerminal());
        if (!next) {
            return false;
        }
        target = *next;
    }
    return false;
}

bool JitTranslator::RegionSuccessorOverwritesFlagsToken(
        ir::Location target) const {
    const auto found = region_block_map.find(target.Value());
    if (found == region_block_map.end() || !found->second) {
        return false;
    }
    ir::Flags needed = ir::Flags::Parity | ir::Flags::AuxiliaryCarry;
    for (auto& inst : found->second->GetInstList()) {
        if (MayFaultOrObserve(inst) ||
            HelperCallContract::InstructionClobbersGPR(
                    inst, atomic_scratch.GetCode(), context.GetFeatures())) {
            return false;
        }
        if (inst.GetOp() == ir::OpCode::GetFlags ||
            inst.GetOp() == ir::OpCode::TestFlags ||
            inst.GetOp() == ir::OpCode::TestNotFlags) {
            return false;
        }
        if (inst.GetOp() == ir::OpCode::SaveFlags) {
            needed &= ~inst.GetArg<ir::Flags>(1);
        } else if (inst.GetOp() == ir::OpCode::ClearFlags) {
            needed &= ~inst.GetArg<ir::Flags>(0);
        }
        if (!True(needed)) {
            return true;
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            return false;
        }
    }
    return false;
}

JitTranslator::RegionFlagsJoinRecipe JitTranslator::RecipeRegionFlagsJoin(
        ir::Location then_target,
        ir::Location else_target,
        bool allow_fallthrough) const {
    const auto producer = flag_state.save_in_nzcv && flag_state.nzcv_dirty
            ? EdgeFlagsProducer::Arithmetic
            : EdgeFlagsProducer::Restore;
    const auto incoming = PendingEdgeFlagsState(flag_state.nzcv_requested, producer);
    ASSERT(incoming.HasPendingPState());
    const bool then_accepts = RegionSuccessorAcceptsEdgeFlags(
            then_target, incoming);
    const bool else_accepts = RegionSuccessorAcceptsEdgeFlags(
            else_target, incoming);
    if (then_accepts == else_accepts) {
        return {
                .mode = then_accepts ? RegionFlagsJoinMode::Deferred
                                     : RegionFlagsJoinMode::Canonical,
        };
    }
    if (!context.CanUseRegionTrampoline()) {
        return {};
    }

    const auto compatible = then_accepts ? then_target : else_target;
    const auto canonical = then_accepts ? else_target : then_target;
    if (flag_state.flags_token_valid &&
        !RegionSuccessorOverwritesFlagsToken(compatible)) {
        return {};
    }
    if (IsRegionCycleEdge(compatible) || IsDirectCycleCutEdge(compatible) ||
        IsRegionCycleEdge(canonical) || IsDirectCycleCutEdge(canonical)) {
        return {};
    }
    const bool compatible_fallthrough = allow_fallthrough &&
            CanUseRegionSuccessorLayout(compatible);
    const bool canonical_fallthrough = allow_fallthrough &&
            CanUseRegionSuccessorLayout(canonical) &&
            !compatible_fallthrough;
    const bool full_nzcv = incoming.valid_nzcv_mask == kEdgeNZCVMask;
    return {
            .mode = full_nzcv && canonical_fallthrough
                    ? RegionFlagsJoinMode::Split
                    : RegionFlagsJoinMode::CanonicalTail,
            .incoming = incoming,
            .compatible_target = compatible,
            .canonical_target = canonical,
            .compatible_on_true = then_accepts,
            .compatible_fallthrough = compatible_fallthrough,
            .canonical_fallthrough = full_nzcv && canonical_fallthrough,
            .canonical_merge_token = flag_state.flags_token_valid,
    };
}

Label* JitTranslator::GetRegionFlagsCanonicalStub(
        const RegionFlagsJoinRecipe& recipe) {
    RegionFlagsCanonicalStubKey key{
            .target = recipe.canonical_target.Value(),
            .mask = recipe.incoming.valid_nzcv_mask,
            .polarity = recipe.incoming.carry_polarity,
            .token = recipe.canonical_merge_token,
    };
    auto& entry = region_flags_canonical_stubs[key];
    if (!entry) {
        entry = std::make_unique<Label>();
    }
    return entry.get();
}

void JitTranslator::EmitRegionFlagsCanonicalStubs() {
    for (auto& [key, entry] : region_flags_canonical_stubs) {
        __ Bind(entry.get());
        __ Adr(ip1, LocalBranchTarget(ir::Location{key.target}));
        if (key.mask == kEdgeNZCVMask) {
            context.EmitFlagsMergeBranch(
                    key.token ? FlagsMergeTrampolineKind::NZCVToken
                              : FlagsMergeTrampolineKind::NZCV);
        } else {
            context.EmitFlagsMergeBranch(
                    key.token ? FlagsMergeTrampolineKind::NZCVMaskToken
                              : FlagsMergeTrampolineKind::NZCVMask,
                    static_cast<u8>(key.mask >> 28));
        }
    }
    region_flags_canonical_stubs.clear();
}

bool JitTranslator::EmitRegionFlagsJoin(
        const RegionFlagsJoinRecipe& recipe,
        const std::function<void(Label*, bool)>& branch) {
    if (recipe.mode == RegionFlagsJoinMode::Canonical) {
        MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
                  FlagsRegsAuditEdgeKind::RegionInternal);
        return false;
    }
    if (recipe.mode == RegionFlagsJoinMode::Deferred) {
        PublishFlagsToken();
        return false;
    }

    if (recipe.mode == RegionFlagsJoinMode::CanonicalTail) {
        if (recipe.canonical_merge_token) {
            MaterializeFlagsTokenResult();
        }
        branch(GetRegionFlagsCanonicalStub(recipe),
               !recipe.compatible_on_true);
        context.RecordExecCounter(exec_offset_exit_direct);
        context.RecordExecCounter(exec_offset_region_edges);
        ++statistics.region_block_edges;
        EmitRegionEdge(recipe.compatible_target,
                       recipe.compatible_fallthrough,
                       false,
                       false);
        return true;
    }

    branch(LocalBranchTarget(recipe.compatible_target),
           recipe.compatible_on_true);
    context.RecordExecCounter(exec_offset_exit_direct);
    context.RecordExecCounter(exec_offset_region_edges);
    ++statistics.region_block_edges;
    (void)EmitOutlinedNZCVMergeResume(recipe.canonical_merge_token);
    EmitRegionEdge(recipe.canonical_target,
                   recipe.canonical_fallthrough,
                   false,
                   false);
    return true;
}

#undef __

}  // namespace swift::runtime::backend::arm64
