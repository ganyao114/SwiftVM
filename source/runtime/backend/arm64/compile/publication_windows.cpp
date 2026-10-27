#include <algorithm>
#include <iterator>
#include "base/scope_exit.h"
#include "fixed_validation.h"
#include "residence_preparation.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/width_contracts.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/common/variant_util.h"
#include "runtime/compile/allocate/scratch.h"
#include "runtime/compile/allocate/transaction.h"
#include "runtime/compile/analysis/uses.h"

namespace swift::runtime::backend::arm64 {

bool ResidencePreparation::AppendRecolorValue(Vector<u32>& values, const ir::Inst* inst) const {
    if (!inst) return true;
    const auto value = ir::ResolveBitCastSource(ir::Value{inst});
    auto* root = value.Def();
    if (!root || !UseFacts().Contains(root) || ir::IsFloatValueType(root->ReturnType()) ||
        !root->HasValue() || ir::GetValueSizeByte(root->ReturnType()) > 8)
        return false;
    if (std::find(values.begin(), values.end(), root->Id()) != values.end()) return true;
    for (auto* anchor : block->GetLoopHoistMetadata().anchors)
        if (anchor == root) return false;
    values.push_back(root->Id());
    return true;
}

bool ResidencePreparation::RecolorPublication(std::span<const u32> ids,
                                              u16 home,
                                              u32 publication,
                                              u32 last_use) {
    if (ids.empty() || !IsFixedGPRHome(home) || !allocation.GetGprs().Get(home)) return false;
    const auto& index = UseFacts();
    u32 begin = UINT32_MAX, end = last_use;
    // Callers use AppendRecolorValue to prove and canonicalize each local
    // semantic root, retaining its original order and first owner.
    for (const auto id : ids) {
        ASSERT(id < allocation.MapCount());
        begin = std::min(begin, id);
        end = std::max(end, allocation.LiveEnd(id));
    }
    if (end > block->GetInstList().back().Id()) return false;
    const auto member = [&](ir::Value value) {
        return value.Defined() &&
               std::find(ids.begin(), ids.end(), allocation.AllocationId(value)) != ids.end();
    };
    bool needs_tail = false;
    compile::VisitTerminalValues(block->GetTerminal(), [&](ir::Value value) {
        if (!needs_tail) needs_tail = member(value);
    });
    if (begin > publication || publication > end) return false;
    const auto first = ir::Value{index.FindInstruction(ids.front())};
    if (!register_state.PreparePublicationWindow(home, first, begin, publication) ||
        !register_state.PrepareHomeSurvival(home, publication, end))
        return false;
    const bool scratch_only = X86PinExtScratchOnlyEnabled(allocation.GetGprs(), features);
    Vector<compile::GPRRecolorSite> sites;
    for (auto* inst : index.Instructions()) {
        const auto id = inst->Id();
        const auto value = inst->HasValue() ? ir::Value{inst} : ir::Value{};
        const bool own = member(value);
        const bool later_read = inst->GetOp() == ir::OpCode::GetHostGPR && id >= publication &&
                                inst->GetArg<ir::Imm>(0).Get() == home;
        if (value.Defined() && !own && !later_read) {
            const auto* fixed = allocation.FixedGPRDefinitionAt(id);
            if (fixed && ((fixed->home == home && id <= end && fixed->end > begin) ||
                          (fixed->transferred_home == home && fixed->transfer < end &&
                           fixed->last_use > begin)))
                return false;
            if (allocation.ValueType(value) == compile::AllocationWriter::GPR &&
                allocation.ValueGPR(value).id == home && id <= end &&
                allocation.LiveEnd(id) > begin && (id < end || needs_tail))
                return false;
        }
        if (id < begin || id > end) continue;
        auto clobbers = FixedGPRClobbers(*inst, features, scratch_only);
        if (inst->GetOp() == ir::OpCode::SetHostGPR && !allocation.IsHostWriteCoalesced(id))
            clobbers |= 1u << inst->GetArg<ir::Imm>(1).Get();
        if (const auto* fixed = allocation.FixedGPRDefinitionAt(id); fixed && !fixed->elided)
            clobbers |= 1u << fixed->home;
        if (id == block->GetInstList().back().Id() && ScratchXPoolEnabled(features))
            clobbers |= kTerminalFixedGPRClobbers;
        if (id > begin && id < end && id != publication && !own &&
            ((clobbers & (1u << home)) ||
             HelperCallContract::InstructionClobbersGPR(*inst, home, features)))
            return false;
        bool conflict = false;
        inst->ForEachValue([&](ir::Value input) {
            if (id == begin || member(input)) return;
            const auto location = allocation.GPRLocationAt(input, id);
            auto* root = ir::ResolveBitCastSource(input).Def();
            const bool same_version = root && root->GetOp() == ir::OpCode::GetHostGPR &&
                                      root->Id() >= publication &&
                                      root->GetArg<ir::Imm>(0).Get() == home;
            conflict |= !same_version && location.kind == compile::AllocationWriter::GPR &&
                        location.slot == home;
        });
        if (conflict) return false;
        sites.push_back({id, clobbers});
    }
    compile::AllocationEdit edit{allocation, begin, end + 1};
    const auto refusal =
            edit.RecolorGPR(ids, home, FixedGPRClassEnabled(allocation.GetGprs(), features), sites);
    if (refusal != compile::RecolorFailure::None) return false;
    // The matched publication is part of this edit: the producer and its
    // elided views now supply exactly the bits that the store would publish.
    allocation.MarkHostWriteCoalesced(publication);
    for (size_t i = 0; i < ids.size(); ++i) {
        auto* inst = index.FindInstruction(ids[i]);
        const bool elided = i != 0;
        const auto last = allocation.LiveEnd(inst->Id());
        const auto* fixed = allocation.FixedGPRDefinitionAt(inst->Id());
        if (fixed && fixed->home == home && fixed->end >= last && fixed->elided == elided) continue;
        allocation.PlanFixedGPRDefinition(
                inst->Id(),
                {.home = home,
                 .end = last,
                 .last_use = last,
                 .publication = publication,
                 .owner = ids.front(),
                 .width = static_cast<u8>(ir::GetValueSizeByte(inst->ReturnType())),
                 .elided = elided,
                 .version = true});
    }
    const auto terminal_extra = compile::CountTerminalReloads(allocation, block->GetTerminal());
    for (auto* inst : index.Instructions()) {
        if (inst->Id() < begin || inst->Id() > end) continue;
        compile::FlagsDemand demand;
        // A zero packed mask is insufficient: SaveFlags(None) still has a
        // pseudo use. Only the maintained use counts prove this query empty.
        if (inst->GetUses(false) != inst->GetUses()) {
            use_facts->PrepareUses();
            index.ForEachPseudoUse(inst, [&](const ir::Inst& pseudo) {
                if (pseudo.GetOp() != ir::OpCode::SaveFlags &&
                    pseudo.GetOp() != ir::OpCode::BranchOnlyFlags)
                    return;
                demand.requested |= pseudo.GetArg<ir::Flags>(1);
                ++demand.pseudo_count;
                demand.branch_only |= pseudo.GetOp() == ir::OpCode::BranchOnlyFlags;
                demand.has_save |= pseudo.GetOp() == ir::OpCode::SaveFlags;
            });
        }
        const auto extra =
                inst == &block->GetInstList().back() ? terminal_extra : compile::ScratchReloads{};
        if (!compile::CheckInstructionScratch(
                    allocation,
                    *inst,
                    DescribeInstructionScratch(allocation.GetGprs(), features, *inst, demand),
                    extra))
            return false;
    }
    {
        // This witness belongs only to the current proof. Reuse the existing
        // write buffer, restoring its logical contents on rejection or throw.
        target_writes.push_back({ids.front(), home});
        const auto restore_writes = ScopeExit([&] { target_writes.pop_back(); });
        FixedValidationDescription described{block, allocation, features, index};
        if (!compile::CheckFixedConstraints(block, allocation, described.Inputs(), target_writes))
            return false;
    }
    use_facts->PrepareResolvedUses();
    if (!CheckWidthContracts(block, allocation, features, uniforms, index)) return false;
    edit.Commit();
    return true;
}

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool IsFullGPRWrite(const ir::Inst& inst) {
    if (inst.GetOp() != ir::OpCode::SetHostGPR || inst.GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const auto width = ir::GetValueSizeByte(inst.GetArg<ir::Value>(0).Type());
    return width == sizeof(u32) || width == sizeof(u64);
}

bool IsWriteBoundary(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::SetLocation:
        case ir::OpCode::GetUniformAddress:
        case ir::OpCode::UniformBarrier:
        case ir::OpCode::Goto:
        case ir::OpCode::NotGoto:
        case ir::OpCode::BindLabel:
            return true;
        default:
            return false;
    }
}

const ir::Inst* PublicationRoot(const ir::Inst& store) {
    auto value = store.GetArg<ir::Value>(0);
    auto* root = value.Def();
    while (root && (root->IsBitCastOperation() || root->GetOp() == ir::OpCode::ZeroExtend32To64)) {
        value = root->GetArg<ir::Value>(0);
        root = value.Def();
    }
    return root;
}

}  // namespace

bool ResidencePreparation::IsPublicationCandidate(const ir::Inst& inst) {
    // The five matchers share these gates. Do not filter by value width:
    // some existing matchers validate the underlying producer instead.
    return inst.GetOp() == ir::OpCode::SetHostGPR && inst.GetArg<ir::Imm>(2).Get() == 0 &&
           IsFixedGPRHome(inst.GetArg<ir::Imm>(1).Get());
}

bool ResidencePreparation::IsDeadPinnedGPRWrite(const ir::Inst* inst) const {
    ASSERT(block);
    if (!inst || inst->GetOp() != ir::OpCode::SetHostGPR ||
        (allocation.IsHostWriteCoalesced(inst->Id()) ||
         allocation.NeedsFixedGPRPublication(inst->Id()))) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    if (!IsFixedGPRHome(target)) {
        return false;
    }

    auto* next = write_successors.FollowingWrite(inst);
    if (!next) return false;
    auto& overwrite = *next;
    // The index only removes the search. Recheck actual width, allocation
    // and publication-root ordering before suppressing the write.
    if (!IsFullGPRWrite(overwrite)) return false;
    if (!allocation.IsHostWriteCoalesced(overwrite.Id())) return true;
    auto* root = PublicationRoot(overwrite);
    return root && root->Id() > inst->Id();
}

void ResidencePreparation::PrepareDeadPinnedGPRWrites(bool seed_candidates) {
    write_successors.Build(
            UseFacts(),
            [&](const ir::Inst& inst) {
                return MayFaultOrObserve(inst) || IsWriteBoundary(inst.GetOp());
            },
            IsFullGPRWrite);
    StackVector<RegisterStatePreparation::CoalescedWrite, 8> coalesced_writes;
    // Published versions per block are few; linear membership
    // avoids a hash set allocation for every prepared block.
    StackVector<const ir::Inst*, 16> published_versions;
    const auto version_published = [&](const ir::Inst* version) {
        for (auto* item : published_versions)
            if (item == version) return true;
        return false;
    };
    const bool want_deadwrite = GetSvmConfig().skip_prep.find("deadwrite") == std::string::npos;
    bool has_reused_publication = false;
    const auto instructions = UseFacts().Instructions();
    for (u32 position = 0; position < instructions.size(); ++position) {
        const auto& inst = *instructions[position];
        if (!has_reused_publication && !published_versions.empty()) {
            inst.ForEachValue([&](ir::Value value) {
                if (!has_reused_publication && value.Def() && version_published(value.Def()))
                    has_reused_publication = true;
            });
        }
        const bool dead = want_deadwrite && IsDeadPinnedGPRWrite(&inst);
        if (dead) {
            dead_publications.push_back(&inst);
        }
        if (!IsPublicationCandidate(inst) || dead) {
            continue;
        }
        if (seed_candidates) publication_index.push_back(position);
        const u32 home = inst.GetArg<ir::Imm>(1).Get();
        auto published = inst.GetArg<ir::Value>(0);
        if (!has_reused_publication && published.Def()) {
            auto* version = published.Def();
            published_versions.push_back(version);
            while (version && (version->GetOp() == ir::OpCode::ZeroExtend32 ||
                               version->GetOp() == ir::OpCode::ZeroExtend32To64 ||
                               version->GetOp() == ir::OpCode::SignExtend)) {
                auto alias = version->GetArg<ir::Value>(0);
                version = alias.Def();
                if (version) {
                    published_versions.push_back(version);
                }
            }
        }
        if (IsGPRMappedTo(published, home)) {
            coalesced_writes.push_back({&inst, static_cast<u16>(home)});
        }
    }
    std::sort(dead_publications.begin(), dead_publications.end(), std::less<const ir::Inst*>{});
    register_state.PrepareValueVersions(dead_publications,
                                       std::span<const RegisterStatePreparation::CoalescedWrite>{
                                               coalesced_writes.data(), coalesced_writes.size()},
                                       has_reused_publication);
}

}  // namespace swift::runtime::backend::arm64
