#include "runtime/backend/arm64/region_link_trampoline.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include "aarch64/macro-assembler-aarch64.h"
#include "runtime/backend/arm64/continuation_contract.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/common/alignment.h"
#include "runtime/common/svm_config.h"
#include "runtime/externals/vixl/svm-vixl-prof.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

namespace {

[[nodiscard]] LinkSiteKey SiteKey(const CodeRegion& region, const void* rx_site) {
    return LinkSiteKey{
            .region_id = region.id,
            .offset = static_cast<u32>(static_cast<const u8*>(rx_site) - region.rx_base),
    };
}

[[nodiscard]] void* ReturnToDispatcher(RegionLinkContext& context,
                                       State* state,
                                       const std::optional<LinkSiteRecord>& site) {
    if (state && site) {
        state->current_loc = ir::Location{site->guest_target};
    }
    const auto traversal = site && site->kind == LinkSiteKind::Call
                                   ? ContinuationTraversal::CallMiss
                                   : ContinuationTraversal::Branch;
    return ContinuationContract::EncodeTraversal(context.dispatcher, traversal);
}

}  // namespace

extern "C" void* RegionLinkTrampolineSlow(RegionLinkContext* context,
                                           State* state,
                                           const void* rx_site) {
    if (!context || !context->manager || !context->region ||
        !context->region->ContainsRx(rx_site)) {
        return context ? context->dispatcher : nullptr;
    }
    context->manager->RecordLinkerCall();
    const auto key = SiteKey(*context->region, rx_site);
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        const auto site = context->manager->QuerySite(key);
        if (!site || site->state == LinkSiteState::Retiring) {
            return ReturnToDispatcher(*context, state, site);
        }
        const auto target = context->manager->QueryTarget(site->guest_target);
        const bool call = site->kind == LinkSiteKind::Call;
        if (!target || !target->host_pc || (call && !target->call_host_pc)) {
            return ReturnToDispatcher(*context, state, site);
        }
        auto* direct_host_pc = call
                ? (site->flags_bypass_offset != UINT32_MAX &&
                                   target->pending_flags_contract.Accepts(
                                           site->edge_flags) &&
                                   target->call_pending_flags_host_pc
                           ? target->call_pending_flags_host_pc
                           : target->call_host_pc)
                : (site->flags_bypass_offset != UINT32_MAX &&
                                   target->pending_flags_contract.Accepts(
                                           site->edge_flags) &&
                                   target->pending_flags_host_pc
                           ? target->pending_flags_host_pc
                           : (target->direct_host_pc ? target->direct_host_pc
                                                     : target->host_pc));
        auto* traversal_host_pc = call ? target->call_host_pc : target->host_pc;

        if (site->state == LinkSiteState::Linked) {
            if (site->target_generation == target->generation) {
                return ContinuationContract::EncodeTraversal(
                        traversal_host_pc,
                        call ? ContinuationTraversal::Call : ContinuationTraversal::Branch);
            }
            continue;
        }
        if (site->state == LinkSiteState::Far) {
            if (site->target_generation == target->generation) {
                return ContinuationContract::EncodeTraversal(
                        traversal_host_pc,
                        call ? ContinuationTraversal::Call : ContinuationTraversal::Branch);
            }
            continue;
        }

        const bool same_region = target->region_id == context->region->id &&
                                 context->region->ContainsRx(direct_host_pc);
        if (!same_region || !Imm26Reachable(rx_site, direct_host_pc)) {
            if (context->manager->MarkFar(key, target->generation)) {
                return ContinuationContract::EncodeTraversal(
                        traversal_host_pc,
                        call ? ContinuationTraversal::Call : ContinuationTraversal::Branch);
            }
            continue;
        }

        const auto offset = static_cast<u8*>(direct_host_pc) -
                            static_cast<const u8*>(rx_site);
        const auto branch = call ? EncodeBL(offset) : EncodeB(offset);
        if (!branch) {
            return ReturnToDispatcher(*context, state, site);
        }
        const bool linked = context->manager->MarkLinked(
                key, target->generation, [&](const LinkSiteRecord&) {
                    auto* rw_site = SiteRxToRw(*context->region, rx_site);
                    return rw_site && PatchDirectBranch(
                                              *context->region,
                                              const_cast<void*>(rx_site),
                                              rw_site,
                                              *branch);
                });
        if (linked) {
            return ContinuationContract::EncodeTraversal(
                    traversal_host_pc,
                    call ? ContinuationTraversal::Call : ContinuationTraversal::Branch);
        }
    }
    return ReturnToDispatcher(*context, state, context->manager->QuerySite(key));
}

RegionLinkTrampolineCode BuildRegionLinkTrampoline(
        const Config& config,
        RegionLinkContext* context,
        const FeatureSet& features) {
    vixl::svm_vixl_prof::JitScope vixl_prof{features.vixl_fast};
    MacroAssembler masm;
    std::vector<u16> gprs;
    std::vector<u16> fprs;
    for (const auto& desc : config.buffers_static_alloc) {
        if (!desc.is_float && desc.reg <= 9) {
            gprs.push_back(desc.reg);
        } else if (desc.is_float && desc.reg >= 16 && desc.reg <= 31) {
            fprs.push_back(desc.reg);
        }
    }
    std::sort(gprs.begin(), gprs.end());
    gprs.erase(std::unique(gprs.begin(), gprs.end()), gprs.end());
    std::sort(fprs.begin(), fprs.end());
    fprs.erase(std::unique(fprs.begin(), fprs.end()), fprs.end());

    constexpr u32 kFrameHeader = 32;  // saved x29/x30, helper result, padding
    const u32 gpr_base = kFrameHeader;
    const u32 fpr_base = AlignUp(
            gpr_base + static_cast<u32>(gprs.size() * sizeof(u64)), size_t{16});
    const u32 frame_size = static_cast<u32>(
            AlignUp(fpr_base + fprs.size() * sizeof(u128), size_t{16}));

    Label canonical_entry;
    Label return_host;
    const u32 pending_flags_offset =
            static_cast<u32>(masm.GetBuffer()->GetSizeInBytes());
    masm.Mrs(x16, NZCV);
    masm.And(x26, x26, ~u64{0xf0000000});
    masm.Orr(x26, x26, x16);
    masm.B(&canonical_entry);
    ASSERT(masm.GetBuffer()->GetSizeInBytes() ==
           pending_flags_offset + kFlagsMergeOffsetFromPending);
    masm.Mrs(x16, NZCV);
    masm.And(x26, x26, ~u64{0xf0000000});
    masm.Orr(x26, x26, x16);
    masm.Br(x17);
    ASSERT(masm.GetBuffer()->GetSizeInBytes() ==
           pending_flags_offset + kFlagsMergeTokenOffsetFromPending);
    masm.Mrs(x16, NZCV);
    masm.And(x26, x26, ~u64{0xf0000000});
    masm.Orr(x26, x26, x16);
    masm.Bfi(x26, x12, 0, 8);
    masm.Br(x17);
    ASSERT(masm.GetBuffer()->GetSizeInBytes() ==
           pending_flags_offset + kReturnFlagsMergeOffsetFromPending);
    masm.Mrs(x16, NZCV);
    masm.And(x26, x26, ~u64{0xf0000000});
    masm.Orr(x26, x26, x16);
    masm.B(&return_host);
    ASSERT(masm.GetBuffer()->GetSizeInBytes() ==
           pending_flags_offset + kReturnFlagsMergeTokenOffsetFromPending);
    masm.Mrs(x16, NZCV);
    masm.And(x26, x26, ~u64{0xf0000000});
    masm.Orr(x26, x26, x16);
    masm.Bfi(x26, x12, 0, 8);
    masm.B(&return_host);
    ASSERT(masm.GetBuffer()->GetSizeInBytes() ==
           pending_flags_offset + kCycleFlagsMergeOffsetFromPending);
    Label cycle_reason;
    masm.Mrs(x16, NZCV);
    masm.And(x26, x26, ~u64{0xf0000000});
    masm.Orr(x26, x26, x16);
    masm.B(&cycle_reason);
    ASSERT(masm.GetBuffer()->GetSizeInBytes() ==
           pending_flags_offset + kCycleFlagsMergeTokenOffsetFromPending);
    masm.Mrs(x16, NZCV);
    masm.And(x26, x26, ~u64{0xf0000000});
    masm.Orr(x26, x26, x16);
    masm.Bfi(x26, x12, 0, 8);
    masm.B(&cycle_reason);
    ASSERT(masm.GetBuffer()->GetSizeInBytes() ==
           pending_flags_offset + kCycleReasonOffsetFromPending);
    masm.Bind(&cycle_reason);
    Label signal;
    Label publish;
    masm.Ldar(x16, MemOperand(x28, offsetof(State, exit_request)));
    masm.Tbnz(x16, 63, &signal);
    masm.Mov(w17, static_cast<u32>(HaltReason::CodeMiss));
    masm.B(&publish);
    masm.Bind(&signal);
    masm.Mov(w17, static_cast<u32>(HaltReason::Signal));
    masm.Bind(&publish);
    masm.Str(w17, MemOperand(x28, offsetof(State, halt_reason)));
    masm.B(&return_host);
    masm.Bind(&canonical_entry);
    const u32 canonical_offset =
            static_cast<u32>(masm.GetBuffer()->GetSizeInBytes());
    masm.Sub(sp, sp, frame_size);
    masm.Stp(x29, x30, MemOperand(sp, 0));
    masm.Str(x14, MemOperand(sp, 24));
    for (u32 i = 0; i < gprs.size(); ++i) {
        masm.Str(XRegister(gprs[i]), MemOperand(sp, gpr_base + i * sizeof(u64)));
    }
    for (u32 i = 0; i < fprs.size(); ++i) {
        masm.Str(VRegister::GetQRegFromCode(fprs[i]),
                 MemOperand(sp, fpr_base + i * sizeof(u128)));
    }

    // BL set LR to site+4. Preserve that direct before setting up AAPCS64
    // arguments. x28 is the permanent State register in the JIT ABI.
    masm.Ldr(x2, MemOperand(sp, 8));
    masm.Sub(x2, x2, 4);
    masm.Mov(x0, reinterpret_cast<uintptr_t>(context));
    masm.Mov(x1, x28);
    if (config.sse_afp_nan) {
        masm.Ldr(x11, MemOperand(sp, frame_size + kSseAFPHostFPCROffset));
        masm.Msr(FPCR, x11);
    }
    masm.Mov(x11, reinterpret_cast<uintptr_t>(&RegionLinkTrampolineSlow));
    masm.Blr(x11);
    masm.Str(x0, MemOperand(sp, 16));

    if (config.sse_afp_nan) {
        EmitSseAFPRestoreGuestFPCRCached(
                masm, x28, frame_size, x11, x16, x17);
    }
    for (u32 i = 0; i < fprs.size(); ++i) {
        masm.Ldr(VRegister::GetQRegFromCode(fprs[i]),
                 MemOperand(sp, fpr_base + i * sizeof(u128)));
    }
    for (u32 i = 0; i < gprs.size(); ++i) {
        masm.Ldr(XRegister(gprs[i]), MemOperand(sp, gpr_base + i * sizeof(u64)));
    }
    masm.Ldr(x14, MemOperand(sp, 24));
    masm.Ldp(x29, x30, MemOperand(sp, 0));
    masm.Ldr(x16, MemOperand(sp, 16));
    masm.Add(sp, sp, frame_size);
    Label continuation_published;
    masm.Tbz(x16, ContinuationContract::kPublishFrameBit, &continuation_published);
    ContinuationContract::PublishFrame(masm);
    masm.Bind(&continuation_published);
    Label call_target;
    masm.Tbnz(x16, ContinuationContract::kCallTraversalBit, &call_target);
    masm.Mov(x30, reinterpret_cast<uintptr_t>(context->return_host));
    masm.Bind(&call_target);
    masm.Bic(x16, x16, ContinuationContract::kTraversalTagMask);
    // A branch-instruction patch is not guaranteed to be observed merely by
    // cache maintenance performed on another core. The first slow traversal
    // performs context synchronization before entering the selected target.
    masm.Isb();
    if (FlagsRegsEnabled()) {
        masm.Msr(NZCV, x26);
    }
    masm.Br(x16);
    const u32 return_offset =
            static_cast<u32>(masm.GetBuffer()->GetSizeInBytes());
    masm.Bind(&return_host);
    masm.Mov(x16, reinterpret_cast<uintptr_t>(context->return_host));
    masm.Br(x16);

    std::array<u32, 16> flags_mask_offsets{};
    std::array<u32, 16> flags_mask_token_offsets{};
    auto emit_mask_merge = [&](u32 mask, bool token) {
        auto& offsets = token ? flags_mask_token_offsets : flags_mask_offsets;
        offsets[mask] = static_cast<u32>(masm.GetBuffer()->GetSizeInBytes());
        const u64 requested = static_cast<u64>(mask) << 28;
        const u32 lsb = std::countr_zero(requested);
        const u64 adjusted = requested >> lsb;
        if (!(adjusted & (adjusted + 1))) {
            const u32 width = std::bit_width(adjusted);
            masm.Mrs(x16, NZCV);
            masm.Ubfx(x16, x16, lsb, width);
            masm.Bfi(x26, x16, lsb, width);
        } else {
            masm.Mov(x15, requested);
            masm.Mrs(x16, NZCV);
            masm.Bic(x26, x26, x15);
            masm.And(x16, x16, x15);
            masm.Orr(x26, x26, x16);
        }
        if (token) {
            masm.Bfi(x26, x12, 0, 8);
        }
        masm.Br(x17);
    };
    for (u32 mask = 1; mask < 15; ++mask) {
        emit_mask_merge(mask, false);
        emit_mask_merge(mask, true);
    }
    masm.FinalizeCode();

    const size_t size = masm.GetBuffer()->GetSizeInBytes();
    RegionLinkTrampolineCode result{
            .code = std::vector<u8>(size),
            .canonical_offset = canonical_offset,
            .pending_flags_offset = pending_flags_offset,
            .return_offset = return_offset,
            .flags_mask_offsets = flags_mask_offsets,
            .flags_mask_token_offsets = flags_mask_token_offsets,
    };
    std::memcpy(result.code.data(),
                masm.GetBuffer()->GetStartAddress<u8*>(),
                size);
    return result;
}

}  // namespace swift::runtime::backend::arm64
