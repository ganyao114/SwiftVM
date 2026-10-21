#include "translator.h"

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

bool JitTranslator::RegionSuccessorAcceptsEdgeFlags(
        ir::Location target,
        const EdgeFlagsState& incoming) const {
    const auto found = RegionBlockMap().find(target.Value());
    return found != RegionBlockMap().end() &&
           (found->second.accepted_masks & (u16{1} << (incoming.valid_nzcv_mask >> 28))) != 0;
}

bool JitTranslator::RegionSuccessorOverwritesFlagsToken(ir::Location target) const {
    const auto found = RegionBlockMap().find(target.Value());
    return found != RegionBlockMap().end() && found->second.token_overwritten;
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
        Label compatible;
        branch(&compatible, recipe.compatible_on_true);
        RecordExitPollFault(
                context.PollTerminalExit(recipe.canonical_target, GetTerminalRequestExit()),
                GetTerminalRequestExit(), recipe.canonical_target);
        __ B(GetRegionFlagsCanonicalStub(recipe));
        __ Bind(&compatible);
        context.RecordExecCounter(exec_offset_exit_direct);
        context.RecordExecCounter(exec_offset_region_edges);
        ++statistics.region_block_edges;
        EmitRegionEdge(recipe.compatible_target,
                       recipe.compatible_fallthrough,
                       false,
                       false);
        return true;
    }

    Label canonical;
    branch(&canonical, !recipe.compatible_on_true);
    RecordExitPollFault(
            context.PollTerminalExit(recipe.compatible_target, GetTerminalRequestExit()),
            GetTerminalRequestExit(), recipe.compatible_target);
    __ B(LocalBranchTarget(recipe.compatible_target));
    __ Bind(&canonical);
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
