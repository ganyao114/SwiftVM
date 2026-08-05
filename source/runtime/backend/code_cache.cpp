//
// Created by 甘尧 on 2023/9/27.
//

#include <atomic>
#include <cstring>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/arm64/region_link_trampoline.h"
#include "runtime/backend/host_isa.h"
#include "runtime/common/alignment.h"

namespace swift::runtime::backend {

namespace {

std::atomic<CodeRegionId> next_region_id{1};

[[nodiscard]] bool Contains(const u8* base, u32 capacity, const void* address) {
    if (!base || !address) {
        return false;
    }
    const auto begin = reinterpret_cast<uintptr_t>(base);
    const auto value = reinterpret_cast<uintptr_t>(address);
    return value >= begin && value - begin < capacity;
}

}  // namespace

bool CodeRegion::ContainsRx(const void* address) const {
    return Contains(rx_base, capacity, address);
}

bool CodeRegion::ContainsRw(const void* address) const {
    return Contains(rw_base, capacity, address);
}

u8* SiteRxToRw(const CodeRegion& region, const void* rx_site) {
    if (!region.ContainsRx(rx_site)) {
        return nullptr;
    }
    const auto offset =
            reinterpret_cast<uintptr_t>(rx_site) - reinterpret_cast<uintptr_t>(region.rx_base);
    return region.rw_base + offset;
}

u8* SiteRwToRx(const CodeRegion& region, const void* rw_site) {
    if (!region.ContainsRw(rw_site)) {
        return nullptr;
    }
    const auto offset =
            reinterpret_cast<uintptr_t>(rw_site) - reinterpret_cast<uintptr_t>(region.rw_base);
    return region.rx_base + offset;
}

bool SameRegion(const CodeRegion& lhs, const CodeRegion& rhs) {
    return lhs.id != 0 && lhs.id == rhs.id;
}

bool SameRegion(const CodeRegion& region, const void* rx_a, const void* rx_b) {
    return region.ContainsRx(rx_a) && region.ContainsRx(rx_b);
}

bool Imm26Reachable(const void* site, const void* target) {
    constexpr uintptr_t kMaxDistance = (uintptr_t{1} << 27) - 4;
    if (!site || !target) {
        return false;
    }
    const auto source = reinterpret_cast<uintptr_t>(site);
    const auto destination = reinterpret_cast<uintptr_t>(target);
    if ((source & 3u) != 0 || (destination & 3u) != 0) {
        return false;
    }
    return destination >= source ? destination - source <= kMaxDistance
                                 : source - destination <= kMaxDistance;
}

CodeCache::CodeCache(const Config& config, u32 size,
                     const FeatureSet& features, bool read_only)
        : config(config)
        , features(features)
        , max_size(size)
        , inst_alignment(GetInstructionSetInstructionAlignment(config.backend_isa))
        , read_only(read_only) {
    Init();
}

CodeCache::~CodeCache() {
    if (space_code) {
        destroy_mspace(space_code);
    }
}

bool CodeCache::InitializeRegionTrampoline(LinkManager& manager,
                                            void* return_host,
                                            void* dispatcher) {
    constexpr u32 kImm26Window = 1u << 27;
    if (region_link_context_) {
        return true;
    }
    // Merely placing a trampoline "inside" a region is insufficient once the
    // region spans more than one imm26 window. Production callers re-emit
    // such a unit with the dispatch-slot leaf.
    if (config.backend_isa != kArm64 || !return_host || !dispatcher ||
        region.capacity > kImm26Window) {
        return false;
    }

    auto context = std::make_unique<arm64::RegionLinkContext>();
    context->manager = &manager;
    context->region = &region;
    context->return_host = return_host;
    context->dispatcher = dispatcher;
    auto code = arm64::BuildRegionLinkTrampoline(config, context.get(), features);
    const auto buffer = AllocCode(code.size());
    if (!buffer) {
        return false;
    }
    std::memcpy(buffer->rw_data, code.data(), code.size());
    buffer->Flush();
    region.trampoline_offset = buffer->offset;
    region_link_context_ = std::move(context);
    return true;
}

void* CodeCache::GetRegionTrampoline() const {
    if (region.trampoline_offset == CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.trampoline_offset;
}

void CodeCache::Init() {
    code_mem = std::make_unique<MemMap>(max_size, true);
#if defined(__APPLE__) || defined(__linux__)
    code_mem_mapped = reinterpret_cast<u8*>(code_mem->Map(code_mem->GetSize(), 0, MemMap::ReadExe));
#endif

    region = CodeRegion{
            .id = next_region_id.fetch_add(1, std::memory_order_relaxed),
            .rw_base = code_mem->GetMemory(),
            .rx_base = code_mem_mapped ? code_mem_mapped : code_mem->GetMemory(),
            .capacity = max_size,
            .trampoline_offset = CodeRegion::kInvalidTrampolineOffset,
    };

    if (!read_only) {
        space_code = create_mspace_with_base(code_mem->GetMemory(), code_mem->GetSize(), 0);
    } else {
        code_cursor = code_mem->GetMemory();
    }
}

std::optional<CodeBuffer> CodeCache::AllocCode(size_t size) {
    u8* result{};
    if (read_only) {
        code_cursor = reinterpret_cast<u8*>(
                AlignUp(reinterpret_cast<uintptr_t>(code_cursor), inst_alignment));
        if (code_cursor + size <= (code_mem->GetMemory() + max_size)) {
            result = code_cursor;
        }
    } else {
        result = reinterpret_cast<u8*>(mspace_memalign(space_code, inst_alignment, size));
        // create_mspace_with_base grows from the system when its supplied
        // arena is exhausted. JIT code outside this dual-mapped CodeRegion
        // has no RX alias and can also escape the region trampoline's imm26
        // window, so reject and release such a chunk.
        const auto base = reinterpret_cast<uintptr_t>(code_mem->GetMemory());
        const auto address = reinterpret_cast<uintptr_t>(result);
        if (result &&
            (address < base || size > max_size || address - base > max_size - size)) {
            mspace_free(space_code, result);
            result = nullptr;
        }
    }
    if (!result) {
        return std::nullopt;
    }

    CodeBuffer result_buffer{
            result, result, static_cast<u32>(result - code_mem->GetMemory()), size};
#if defined(__APPLE__) || defined(__linux__)
    if (code_mem_mapped) {
        result_buffer.exec_data = result - code_mem->GetMemory() + code_mem_mapped;
    }
#endif
    return result_buffer;
}

bool CodeCache::FreeCode(u8* exec_ptr) {
    ASSERT(!read_only);
#if defined(__APPLE__) || defined(__linux__)
    if (code_mem_mapped) {
        exec_ptr = code_mem->GetMemory() + (exec_ptr - code_mem_mapped);
    }
#endif
    mspace_free(space_code, exec_ptr);
    return true;
}

bool CodeCache::Contain(const u8* exec_ptr) {
#if defined(__APPLE__) || defined(__linux__)
    if (code_mem_mapped) {
        return exec_ptr >= code_mem_mapped && exec_ptr <= (code_mem_mapped + max_size);
    } else
#endif
    {
        return exec_ptr >= code_mem->GetMemory() && exec_ptr <= (code_mem->GetMemory() + max_size);
    }
}

u8* CodeCache::GetExePtr(u32 offset) {
    if (offset > max_size) {
        return nullptr;
    }
#if defined(__APPLE__) || defined(__linux__)
    if (code_mem_mapped) {
        return code_mem_mapped + offset;
    } else
#endif
    {
        return code_mem->GetMemory() + offset;
    }
}

u8* CodeCache::GetRWPtr(u32 offset) {
    if (offset > max_size) {
        return nullptr;
    }
    return code_mem->GetMemory() + offset;
}

u8* CodeCache::GetRWPtr(const u8* exec_ptr) {
#if defined(__APPLE__) || defined(__linux__)
    if (code_mem_mapped) {
        if (exec_ptr < code_mem_mapped || exec_ptr > (code_mem_mapped + max_size)) {
            return nullptr;
        }
        return code_mem->GetMemory() + (exec_ptr - code_mem_mapped);
    } else
#endif
    {
        auto map_start = code_mem->GetMemory();
        if (exec_ptr < map_start || exec_ptr > (map_start + max_size)) {
            return nullptr;
        }
        return const_cast<u8*>(exec_ptr);
    }
}

}  // namespace swift::runtime::backend
