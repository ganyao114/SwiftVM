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

bool JitDiskCache::ReviveRiscvUnit(const std::shared_ptr<Module>& module, const SerialUnit& unit) {
    const auto reject = [&] { ++stats.reject_reloc; dirty = true; return false; };
    bool root{};
    if (unit.blocks.empty() || unit.is_function > 1) return reject();
    std::unordered_set<u64> block_locations;
    for (const auto& block : unit.blocks) {
        if (!block.ValidEntryFlags() ||
            (!block.IsDependency() && !block_locations.insert(block.guest_start).second)) return reject();
        u64 hash;
        if (!HashGuestRange(block.guest_start, block.guest_end, hash) || hash != block.guest_bytes_hash) {
            ++stats.reject_guest_bytes; dirty = true; return false;
        }
        if (block.IsPublished()) {
            if (block.code_offset % 4 || block.code_offset >= unit.code.size()) return reject();
            root |= block.guest_start == unit.guest_start && block.code_offset == 0;
        } else if (block.code_offset != UINT32_MAX || block.IsLinkable()) return reject();
    }
    if (!root || (unit.is_function == 0 && unit.blocks.front().guest_start != unit.guest_start) ||
        (!unit.link_sites.empty() && !module->IsDirectLinkConfigured())) return reject();
    for (const auto& fault : unit.fault_sites)
        if (fault.host_begin % 4 || fault.host_end % 4 || fault.host_begin >= fault.host_end ||
            fault.host_end > unit.code.size() || fault.recovery_kind > u8(FaultRecoveryKind::ExternalContinuation) ||
            (fault.recovery_offset != UINT32_MAX &&
             (fault.recovery_offset % 4 || fault.recovery_offset >= unit.code.size()))) return reject();
    auto [id, buffer] = module->AllocCodeCache(unit.code.size());
    if (id == INVALID_CACHE_ID) { ++stats.reject_alloc; dirty = true; return false; }
    const auto free = [&] { module->GetCodeCache(buffer.exec_data)->FreeCode(buffer.exec_data); return reject(); };
    std::memcpy(buffer.rw_data, unit.code.data(), unit.code.size());
    std::string error;
    if (!ApplyRelocations(buffer.rw_data, buffer.size, unit.relocs, host_image, &error, kRiscv64)) return free();
    buffer.Flush();
    if (!riscv64::RegisterLinkSites(module, buffer, unit.link_sites)) return free();
    ir::AddressNode* node;
    if (unit.is_function) {
        auto* function = new ir::Function(ir::Location{unit.guest_start});
        u64 end = unit.guest_start;
        for (const auto& serial : unit.blocks) {
            if (serial.IsDependency()) continue;
            auto* block = new ir::Block(ir::Location{serial.guest_start});
            block->SetEndLocation(ir::Location{serial.guest_end});
            if (serial.IsPublished()) {
                auto& cache = block->GetJitCache(); cache.jit_state = JitState::Cached;
                cache.cache_id = id; cache.offset_in = buffer.offset + serial.code_offset;
                cache.cache_size = buffer.size - serial.code_offset;
            }
            function->AddBlock(block); end = std::max(end, serial.guest_end);
        }
        function->SetEndLocation(ir::Location{end}); node = function;
    } else {
        auto* block = new ir::Block(ir::Location{unit.guest_start});
        block->SetEndLocation(ir::Location{unit.blocks.front().guest_end}); node = block;
    }
    auto& cache = unit.is_function ? static_cast<ir::Function*>(node)->GetJitCache()
                                   : static_cast<ir::Block*>(node)->GetJitCache();
    cache.jit_state = JitState::Cached;
    cache.cache_id = id; cache.offset_in = buffer.offset; cache.cache_size = buffer.size;
    if (!module->Push(node)) {
        module->DiscardLinkSource(buffer.exec_data);
        if (unit.is_function) delete static_cast<ir::Function*>(node); else delete static_cast<ir::Block*>(node);
        return free();
    }
    for (const auto& fault : unit.fault_sites)
        module->AddFaultEntry(buffer.exec_data + fault.host_begin, buffer.exec_data + fault.host_end,
                fault.guest_start, buffer.exec_data,
                fault.recovery_offset == UINT32_MAX ? nullptr : buffer.exec_data + fault.recovery_offset,
                FaultRecoveryKind(fault.recovery_kind));
    for (const auto& block : unit.blocks) {
        if (block.IsPublished()) {
            auto* entry = buffer.exec_data + block.code_offset;
            address_space.PushCodeCache(ir::Location{block.guest_start}, entry);
            if (block.IsLinkable()) (void)module->PublishLinkTarget(ir::Location{block.guest_start}, entry, buffer.exec_data);
        }
        if (!module->GetModuleConfig().read_only)
            address_space.GetSmcTracker().RegisterNode(module, node, block.guest_start, block.guest_end);
    }
    { std::lock_guard guard(lock); units[unit.guest_start] = unit; }
    ++stats.units_loaded; return true;
}
} // namespace swift::runtime::backend
