#include "link.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/link_manager.h"
#include <cstring>

namespace swift::runtime::backend::riscv64 {
void* ResolveDirectLink(State* state, const u8* rx_site) noexcept {
    try {
        auto* space = reinterpret_cast<AddressSpace*>(state->spill_area[kRiscvAddressSpaceSlot]);
        if (!space) return nullptr;
        auto module = space->GetModule(state->prev_loc.Value());
        if (!module) return nullptr;
        const auto region = module->GetCodeRegion(rx_site);
        if (!region) return nullptr;
        auto& manager = space->GetLinkManager();
        manager.RecordLinkerCall();
        const LinkSiteKey key{region->id, u32(rx_site - region->rx_base)};
        for (u32 attempt = 0; attempt < 3; ++attempt) {
            const auto site = manager.QuerySite(key);
            if (!site || site->state == LinkSiteState::Retiring) return nullptr;
            const auto target = manager.QueryTarget(site->guest_target);
            if (!target || !target->host_pc) return nullptr;
            if ((site->state == LinkSiteState::Linked || site->state == LinkSiteState::Far) &&
                site->target_generation == target->generation) return target->host_pc;
            const auto delta = reinterpret_cast<intptr_t>(target->host_pc) - reinterpret_cast<intptr_t>(rx_site);
            auto branch = EncodeRiscvJal(delta);
            const bool far = !branch;
            if (far) branch = EncodeRiscvJal(4); // Local fixed L2-slot leaf.
            if (manager.MarkLinked(key, target->generation, [&](const auto&) {
                    return PatchDirectBranch(*region, const_cast<u8*>(rx_site), SiteRxToRw(*region, rx_site), *branch);
                }, far)) return target->host_pc;
        }
    } catch (...) { /* A cold linking failure falls back to ordinary dispatch. */ }
    return nullptr;
}

bool RegisterLinkSites(const std::shared_ptr<Module>& module, const CodeBuffer& buffer,
                       std::span<const SerialLinkSite> sites) {
    if (sites.empty()) return true;
    if (!module->IsDirectLinkConfigured()) return false;
    const auto region = module->GetCodeRegion(buffer.exec_data);
    if (!region || region->isa != kRiscv64) return false;
    for (const auto& site : sites) {
        u32 insn{};
        if (site.code_offset % 4 || size_t(site.code_offset) + 4 > buffer.size ||
            site.kind >= u8(LinkSiteKind::Count) || site.HasFlagsBypass() || !site.edge_flags.IsCanonical() ||
            (site.unlinked_instruction & 0xfff) != (0x6f | (28 << 7))) {
            module->DiscardLinkSource(buffer.exec_data); return false;
        }
        std::memcpy(&insn, buffer.rw_data + site.code_offset, 4);
        if (insn != site.unlinked_instruction) { module->DiscardLinkSource(buffer.exec_data); return false; }
        const LinkSignalPatchSite patch{.region = *region,
                .rx_site = buffer.exec_data + site.code_offset, .rw_site = buffer.rw_data + site.code_offset,
                .unlinked_bl = insn};
        if (!module->GetAddressSpace().GetLinkManager().RegisterSite(
                {region->id, buffer.offset + site.code_offset}, site.guest_target,
                {module.get(), buffer.exec_data}, &patch, LinkSiteKind(site.kind))) {
            module->DiscardLinkSource(buffer.exec_data); return false;
        }
    }
    return true;
}

void RecordCode(const std::shared_ptr<Module>& module, const CodeBuffer& buffer,
                u64 guest, bool function, std::vector<SerialBlock> blocks,
                std::span<const SerialLinkSite> links) {
    auto* disk = module->GetAddressSpace().GetJitDiskCache();
    if (!disk) return;
    FaultEntry owner; std::vector<FaultEntry> faults;
    if (!module->LookupCodeAllocation(buffer.exec_data, owner, &faults)) return;
    std::vector<SerialFaultSite> serial_faults;
    for (const auto& fault : faults) serial_faults.push_back({fault.guest_loc,
            u32(fault.host_start - buffer.exec_data), u32(fault.host_end - buffer.exec_data),
            fault.recovery ? u32(fault.recovery - buffer.exec_data) : UINT32_MAX, u8(fault.recovery_kind)});
    disk->RecordUnit(module, guest, function, buffer.exec_data, buffer.rw_data, buffer.size,
                     blocks, {links.begin(), links.end()}, serial_faults);
}
} // namespace swift::runtime::backend::riscv64
