#include "runtime/backend/jit_cache.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/module.h"
#include "runtime/backend/riscv64/link.h"
#include "runtime/ir/function.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace swift::runtime::backend {
void JitDiskCache::RecordRiscvUnit(const std::shared_ptr<Module>& module, VAddr guest,
        bool function, const u8* exec, const u8* rw, u32 size,
        const std::vector<SerialBlock>& blocks, const std::vector<SerialLinkSite>& links,
        const std::vector<SerialFaultSite>& faults) {
    const auto& control = address_space.GetRetirementControl();
    RetirementControl::Lease producer{control, RetirementControl::Kind::Producer};
    if (!producer || !module || module->GetRetirementControl() != control) return;
    SerialUnit unit;
    unit.guest_start = guest; unit.is_function = function;
    unit.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
    unit.blocks = blocks; unit.link_sites = links; unit.fault_sites = faults;
    std::sort(unit.link_sites.begin(), unit.link_sites.end(), [](const auto& a, const auto& b) { return a.code_offset < b.code_offset; });
    unit.code.resize(size);
    size_t cursor{};
    for (const auto& site : unit.link_sites) {
        if (site.code_offset < cursor || size_t(site.code_offset) + 4 > size || site.HasFlagsBypass() ||
            (site.unlinked_instruction & 0xfff) != (0x6f | (28 << 7))) {
            ++stats.reject_scan; return;
        }
        // Never memcpy a word that a live linker can patch concurrently.
        std::memcpy(unit.code.data() + cursor, rw + cursor, site.code_offset - cursor);
        std::memcpy(unit.code.data() + site.code_offset, &site.unlinked_instruction, 4);
        cursor = site.code_offset + 4;
    }
    std::memcpy(unit.code.data() + cursor, rw + cursor, size - cursor);
    const auto scan = ScanCodeUnit(unit.code, host_image,
            address_space.GetConfig().guest_addr_mask ? address_space.GetConfig().guest_addr_mask + 1 : 0,
            {}, kRiscv64);
    if (!scan.ok) { ++stats.reject_scan; return; }
    unit.relocs = scan.relocs;
    for (auto& block : unit.blocks)
        if (!HashGuestRange(block.guest_start, block.guest_end, block.guest_bytes_hash)) { ++stats.reject_scan; return; }
    std::lock_guard guard(lock);
    units[guest] = std::move(unit); dirty = true;
}

} // namespace swift::runtime::backend
