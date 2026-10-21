//
// Created by 甘尧 on 2023/9/27.
//

#include <atomic>
#include <cstring>
#include <new>
#include "base/scope_exit.h"
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
    // Keep the executable alias until module unload invalidates every public
    // entry and guarantees that no Runtime/producer can still use this arena.
    // MemMap closes the backing fd; existing views retain their kernel object.
}

MemMap::ReleaseResult CodeCache::ReleaseAfterQuiescence() {
    std::lock_guard guard(allocation_mutex);
    if (!release_started) {
        release_started = true;
        if (space_code) {
            // Zero unmapped bytes is normal for a fixed external-base arena.
            destroy_mspace(space_code);
            space_code = nullptr;
        }
        region_link_context_.reset();
        region = {};
        code_cursor = nullptr;
        code_mem_mapped = nullptr;
    }
    return code_mem->ReleaseAfterQuiescence();
}

bool CodeCache::InitializeRegionTrampoline(LinkManager& manager,
                                            void* return_host,
                                            void* dispatcher,
                                            const std::shared_ptr<RetirementControl>& control) {
    constexpr u32 kImm26Window = 1u << 27;
    if (release_started) return false;
    if (region_link_context_) {
        return region_link_context_->retirement_control == control;
    }
    // Merely placing a trampoline "inside" a region is insufficient once the
    // region spans more than one imm26 window. Production callers re-emit
    // such a unit with the dispatch-slot leaf.
    if (config.backend_isa != kArm64 || !return_host || !dispatcher || !control ||
        region.capacity > kImm26Window) {
        return false;
    }

    auto context = std::make_unique<arm64::RegionLinkContext>();
    context->manager = &manager;
    context->region = &region;
    context->return_host = return_host;
    context->dispatcher = dispatcher;
    context->retirement_control = control;
    auto trampoline =
            arm64::BuildRegionLinkTrampoline(config, context.get(), features);
    const auto buffer = AllocCode(trampoline.code.size());
    if (!buffer) {
        return false;
    }
    std::memcpy(buffer->rw_data,
                trampoline.code.data(),
                trampoline.code.size());
    buffer->Flush();
    region.trampoline_offset = buffer->offset + trampoline.canonical_offset;
    region.pending_flags_trampoline_offset =
            buffer->offset + trampoline.pending_flags_offset;
    region.return_trampoline_offset = buffer->offset + trampoline.return_offset;
    for (u32 mask = 1; mask < 15; ++mask) {
        flags_mask_trampoline_offsets[mask] =
                buffer->offset + trampoline.flags_mask_offsets[mask];
        flags_mask_token_trampoline_offsets[mask] =
                buffer->offset + trampoline.flags_mask_token_offsets[mask];
    }
    region_link_context_ = std::move(context);
    return true;
}

void* CodeCache::GetRegionTrampoline() const {
    if (region.trampoline_offset == CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.trampoline_offset;
}

void* CodeCache::GetPendingFlagsRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset;
}

void* CodeCache::GetFlagsMergeRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset +
           arm64::kFlagsMergeOffsetFromPending;
}

void* CodeCache::GetFlagsMergeTokenRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset +
           arm64::kFlagsMergeTokenOffsetFromPending;
}

void* CodeCache::GetFlagsMaskMergeRegionTrampoline(u8 mask) const {
    if (mask == 0 || mask >= 15 ||
        region.trampoline_offset == CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + flags_mask_trampoline_offsets[mask];
}

void* CodeCache::GetFlagsMaskMergeTokenRegionTrampoline(u8 mask) const {
    if (mask == 0 || mask >= 15 ||
        region.trampoline_offset == CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + flags_mask_token_trampoline_offsets[mask];
}

void* CodeCache::GetReturnFlagsMergeRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset +
           arm64::kReturnFlagsMergeOffsetFromPending;
}

void* CodeCache::GetReturnFlagsMergeTokenRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset +
           arm64::kReturnFlagsMergeTokenOffsetFromPending;
}

void* CodeCache::GetCycleFlagsMergeRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset +
           arm64::kCycleFlagsMergeOffsetFromPending;
}

void* CodeCache::GetCycleFlagsMergeTokenRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset +
           arm64::kCycleFlagsMergeTokenOffsetFromPending;
}

void* CodeCache::GetCycleReasonRegionTrampoline() const {
    if (region.pending_flags_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.pending_flags_trampoline_offset +
           arm64::kCycleReasonOffsetFromPending;
}

void* CodeCache::GetReturnRegionTrampoline() const {
    if (region.return_trampoline_offset ==
        CodeRegion::kInvalidTrampolineOffset) {
        return nullptr;
    }
    return region.rx_base + region.return_trampoline_offset;
}

void CodeCache::Init() {
    code_mem = std::make_unique<MemMap>(max_size, true);
    auto rollback = ScopeExit([this] {
        if (space_code) {
            destroy_mspace(space_code);
            space_code = nullptr;
        }
        (void)code_mem->ReleaseAfterQuiescence();
        code_mem_mapped = nullptr;
        code_mem.reset();
    });
#if defined(__APPLE__) || defined(__linux__)
    code_mem_mapped = reinterpret_cast<u8*>(code_mem->Map(code_mem->GetSize(), 0, MemMap::ReadExe));
#endif

    region = CodeRegion{
            .id = next_region_id.fetch_add(1, std::memory_order_relaxed),
            .rw_base = code_mem->GetMemory(),
            .rx_base = code_mem_mapped ? code_mem_mapped : code_mem->GetMemory(),
            .capacity = max_size,
            .trampoline_offset = CodeRegion::kInvalidTrampolineOffset,
            .pending_flags_trampoline_offset =
                    CodeRegion::kInvalidTrampolineOffset,
            .return_trampoline_offset = CodeRegion::kInvalidTrampolineOffset,
            .isa = config.backend_isa,
    };

    if (!read_only) {
        space_code = create_mspace_with_base(code_mem->GetMemory(), code_mem->GetSize(), 0);
        if (!space_code) throw std::bad_alloc{};
        if (!mspace_disable_growth(space_code)) throw std::bad_alloc{};
    } else {
        code_cursor = code_mem->GetMemory();
    }
    rollback.Cancel();
}

std::optional<CodeBuffer> CodeCache::AllocCode(size_t size) {
    std::lock_guard guard(allocation_mutex);
    if (release_started || size == 0 || size > max_size) return std::nullopt;
    u8* result{};
    if (read_only) {
        const auto offset = AlignUp(static_cast<size_t>(code_cursor - code_mem->GetMemory()),
                                    inst_alignment);
        if (offset <= max_size && size <= max_size - offset) {
            result = code_mem->GetMemory() + offset;
            code_cursor = result + size;
        }
    } else {
        result = reinterpret_cast<u8*>(mspace_memalign(space_code, inst_alignment, size));
        // Keep a second boundary check even though this arena cannot grow.
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
    std::lock_guard guard(allocation_mutex);
    if (release_started || !region.ContainsRx(exec_ptr)) return false;
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
    return !release_started && region.ContainsRx(exec_ptr);
}

u8* CodeCache::GetExePtr(u32 offset) {
    if (release_started || offset > max_size) {
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
    if (release_started || offset > max_size) {
        return nullptr;
    }
    return code_mem->GetMemory() + offset;
}

u8* CodeCache::GetRWPtr(const u8* exec_ptr) {
    if (release_started) return nullptr;
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
