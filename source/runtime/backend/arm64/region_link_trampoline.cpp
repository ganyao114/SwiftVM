#include "runtime/backend/arm64/region_link_trampoline.h"

#include <algorithm>
#include <cstring>
#include "aarch64/macro-assembler-aarch64.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/common/alignment.h"
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
    return context.dispatcher;
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
        if (!target || !target->host_pc) {
            return ReturnToDispatcher(*context, state, site);
        }

        if (site->state == LinkSiteState::Linked) {
            if (site->target_generation == target->generation) {
                return target->host_pc;
            }
            continue;
        }
        if (site->state == LinkSiteState::Far) {
            if (site->target_generation == target->generation) {
                return target->host_pc;
            }
            continue;
        }

        const bool same_region = target->region_id == context->region->id &&
                                 context->region->ContainsRx(target->host_pc);
        if (!same_region || !Imm26Reachable(rx_site, target->host_pc)) {
            if (context->manager->MarkFar(key, target->generation)) {
                return target->host_pc;
            }
            continue;
        }

        const auto offset = static_cast<u8*>(target->host_pc) - static_cast<const u8*>(rx_site);
        const auto branch = EncodeB(offset);
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
            return target->host_pc;
        }
    }
    return ReturnToDispatcher(*context, state, context->manager->QuerySite(key));
}

std::vector<u8> BuildRegionLinkTrampoline(const Config& config,
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

    masm.Sub(sp, sp, frame_size);
    masm.Stp(x29, x30, MemOperand(sp, 0));
    for (u32 i = 0; i < gprs.size(); ++i) {
        masm.Str(XRegister(gprs[i]), MemOperand(sp, gpr_base + i * sizeof(u64)));
    }
    for (u32 i = 0; i < fprs.size(); ++i) {
        masm.Str(VRegister::GetQRegFromCode(fprs[i]),
                 MemOperand(sp, fpr_base + i * sizeof(u128)));
    }

    // BL set LR to site+4. Preserve that identity before setting up AAPCS64
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
    masm.Ldr(x29, MemOperand(sp, 0));
    masm.Ldr(x16, MemOperand(sp, 16));
    masm.Add(sp, sp, frame_size);
    masm.Mov(x30, reinterpret_cast<uintptr_t>(context->return_host));
    // A branch-instruction patch is not guaranteed to be observed merely by
    // cache maintenance performed on another core. The first slow traversal
    // performs context synchronization before entering the selected target.
    masm.Isb();
    masm.Br(x16);
    masm.FinalizeCode();

    const size_t size = masm.GetBuffer()->GetSizeInBytes();
    std::vector<u8> result(size);
    std::memcpy(result.data(), masm.GetBuffer()->GetStartAddress<u8*>(), size);
    return result;
}

}  // namespace swift::runtime::backend::arm64
