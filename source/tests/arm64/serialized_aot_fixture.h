#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>

#include "aot/aot_artifact.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/context.h"

namespace swift::test {

// Synthetic HIR fixtures do not have an ELF decoder's byte-range census.
// Preserve every emitted entry and recovery recipe using the ordinary codec.
inline aot::AotImage CaptureAotFunctions(runtime::backend::AddressSpace& space,
                                       const void* guest, u64 guest_bytes,
                                       std::span<const u64> locations) {
    using namespace runtime;
    using namespace runtime::backend;
    if (locations.empty()) throw std::runtime_error("empty AOT fixture");
    aot::AotImage image;
    image.host_isa = space.GetConfig().backend_isa;
    image.guest.entry = locations.front();
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(space.GetConfig()), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    for (const auto location : locations) {
        const auto module = space.GetModule(location);
        const auto node = module->GetNode(ir::Location{location});
        if (!IsFunction(node)) throw std::runtime_error("AOT fixture requires a compiled function");
        const auto function = GetFunction(node);
        auto* allocation = static_cast<u8*>(module->GetJitCache(function->GetJitCache()));
        FaultEntry extent;
        if (!module->LookupCodeAllocation(allocation, extent) || extent.host_start != allocation)
            throw std::runtime_error("AOT fixture allocation changed");
        SerialUnit captured;
        std::string error;
        if (!CaptureSerialCode(*module, allocation, u32(extent.host_end - allocation),
                               GetHostImage(), guest_bytes, captured, &error))
            throw std::runtime_error("AOT fixture capture: " + error);
        aot::AotUnit unit;
        unit.guest_start = location;
        unit.is_function = 1;
        unit.code_offset = u32((image.code.size() + 15) & ~size_t{15});
        unit.code_size = u32(captured.code.size());
        unit.relocs = std::move(captured.relocs);
        unit.fault_sites = std::move(captured.fault_sites);
        unit.link_sites = std::move(captured.link_sites);
        unit.region_branches = std::move(captured.region_branches);
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            if (address < allocation || address >= extent.host_end)
                throw std::runtime_error("AOT fixture entry leaves its allocation");
            return u32(address - allocation);
        };
        for (const auto& member : function->GetBlocks()) {
            const auto& block = static_cast<const ir::Block&>(member);
            const auto start = block.GetStartLocation().Value();
            auto* entry = static_cast<const u8*>(space.GetCodeCache(ir::Location{start}));
            if (entry && (entry < allocation || entry >= extent.host_end)) continue;
            if (!entry && block.GetInstList().empty()) continue; // Undecoded external successor.
            const auto end = std::max(start + 1, block.GetEndLocation().Value());
            if (start >= guest_bytes || end > guest_bytes || end <= start)
                throw std::runtime_error("AOT fixture guest range leaves its window");
            SerialBlock record{start, end, UINT32_MAX,
                HashBytes(static_cast<const u8*>(guest) + start, end - start, 0xCBF29CE484222325ull)};
            if (const auto target = space.GetLinkManager().QueryTarget(start)) {
                record.code_offset = offset(target->host_pc);
                record.direct_code_offset = offset(target->direct_host_pc);
                record.pending_flags_code_offset = offset(target->pending_flags_host_pc);
                record.pending_flags_contract = target->pending_flags_contract;
                record.continuation_code_offset = offset(target->call_host_pc);
                record.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
            } else if (entry) {
                record.code_offset = offset(entry);
                record.entry_flags = 0;
            } else record.entry_flags = SerialBlock::MetadataOnly;
            if (!record.ValidEntryContract(unit.code_size)) throw std::runtime_error("invalid captured AOT fixture entry");
            unit.blocks.push_back(record);
        }
        std::sort(unit.blocks.begin(), unit.blocks.end(), [](const auto& a, const auto& b) {
            return a.guest_start < b.guest_start;
        });
        image.code.resize(unit.code_offset);
        image.code.insert(image.code.end(), captured.code.begin(), captured.code.end());
        image.units.push_back(std::move(unit));
    }
    space.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
        image.dispatch_slots.emplace_back(u64(key), index);
    });
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return image;
}

} // namespace swift::test
