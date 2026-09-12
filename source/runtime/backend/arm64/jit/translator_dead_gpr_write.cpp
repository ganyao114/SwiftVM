#include "runtime/backend/reg_alloc.h"
#include "translator.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

bool IsFullGPRWrite(ir::Inst& inst) {
    if (inst.GetOp() != ir::OpCode::SetHostGPR ||
        inst.GetArg<ir::Imm>(2).Get() != 0) {
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

ir::Inst* PublicationRoot(ir::Inst& store) {
    auto value = store.GetArg<ir::Value>(0);
    auto* root = value.Def();
    while (root &&
           (root->IsBitCastOperation() ||
            root->GetOp() == ir::OpCode::ZeroExtend32To64)) {
        value = root->GetArg<ir::Value>(0);
        root = value.Def();
    }
    return root;
}

}  // namespace

bool JitTranslator::IsDeadPinnedGPRWrite(ir::Inst* inst) const {
    ASSERT(pinned_gprs.owner == cur_block);
    if (!inst || inst->GetOp() != ir::OpCode::SetHostGPR ||
        context.IsHostWriteCoalesced(inst->Id())) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    if (!IsFixedGPRHome(target)) {
        return false;
    }

    auto* next = pinned_gprs.block_analysis.FollowingWrite(inst);
    if (!next) return false;
    auto& overwrite = *next;
    // The index only removes the search. Recheck actual width, allocation
    // and publication-root ordering before suppressing the write.
    if (!IsFullGPRWrite(overwrite)) return false;
    if (!context.IsHostWriteCoalesced(overwrite.Id())) return true;
    auto* root = PublicationRoot(overwrite);
    return root && root->Id() > inst->Id();
}

void JitTranslator::PrepareDeadPinnedGPRWrites(ir::Block* block) {
    pinned_gprs.block_analysis.BuildWriteSuccessors(
            [&](ir::Inst& inst) {
                return MayFaultOrObserve(inst) || IsWriteBoundary(inst.GetOp());
            },
            IsFullGPRWrite);
    StackVector<GuestStateMap::CoalescedWrite, 8> coalesced_writes;
    std::unordered_set<ir::Inst*> published_versions;
    bool has_reused_publication = false;
    for (auto& inst : block->GetInstList()) {
        if (!has_reused_publication) {
            for (auto value : inst.GetValues()) {
                if (value.Def() &&
                    published_versions.contains(value.Def())) {
                    has_reused_publication = true;
                    break;
                }
            }
        }
        const bool dead = IsDeadPinnedGPRWrite(&inst) &&
                GetSvmConfig().skip_prep.find("deadwrite") == std::string::npos;
        if (dead) {
            pinned_gprs.dead_pinned_gpr_writes.insert(&inst);
        }
        if (inst.GetOp() != ir::OpCode::SetHostGPR ||
            inst.GetArg<ir::Imm>(2).Get() != 0 || dead) {
            continue;
        }
        const u32 home = inst.GetArg<ir::Imm>(1).Get();
        if (!IsFixedGPRHome(home)) {
            continue;
        }
        auto published = inst.GetArg<ir::Value>(0);
        if (!has_reused_publication && published.Def()) {
            auto* version = published.Def();
            published_versions.insert(version);
            while (version &&
                   (version->GetOp() == ir::OpCode::ZeroExtend32 ||
                    version->GetOp() == ir::OpCode::ZeroExtend32To64 ||
                    version->GetOp() == ir::OpCode::SignExtend)) {
                auto alias = version->GetArg<ir::Value>(0);
                version = alias.Def();
                if (version) {
                    published_versions.insert(version);
                }
            }
        }
        if (context.IsGPRMappedTo(published, home)) {
            coalesced_writes.push_back({&inst, static_cast<u16>(home)});
        }
    }
    guest_state_map.BuildValueVersions(
            pinned_gprs.dead_pinned_gpr_writes,
            std::span<const GuestStateMap::CoalescedWrite>{
                    coalesced_writes.data(), coalesced_writes.size()},
            has_reused_publication);
}

}  // namespace swift::runtime::backend::arm64
