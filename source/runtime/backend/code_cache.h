//
// Created by 甘尧 on 2023/9/27.
//
#pragma once

#include <limits>
#include <memory>
#include <optional>
#include "dlmalloc/malloc.h"
#include "runtime/backend/cache_clear.h"
#include "runtime/backend/mem_map.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"

namespace swift::runtime::backend {

class LinkManager;
namespace arm64 {
struct RegionLinkContext;
}

using CodeRegionId = u64;

struct CodeRegion {
    static constexpr u32 kInvalidTrampolineOffset = std::numeric_limits<u32>::max();

    CodeRegionId id{};
    u8* rw_base{};
    u8* rx_base{};
    u32 capacity{};
    // Offset of the region's shared direct-link cold trampoline, when present.
    u32 trampoline_offset{kInvalidTrampolineOffset};

    [[nodiscard]] bool ContainsRx(const void* address) const;
    [[nodiscard]] bool ContainsRw(const void* address) const;
};

[[nodiscard]] u8* SiteRxToRw(const CodeRegion& region, const void* rx_site);
[[nodiscard]] u8* SiteRwToRx(const CodeRegion& region, const void* rw_site);
[[nodiscard]] bool SameRegion(const CodeRegion& lhs, const CodeRegion& rhs);
[[nodiscard]] bool SameRegion(const CodeRegion& region, const void* rx_a, const void* rx_b);
[[nodiscard]] bool Imm26Reachable(const void* site, const void* target);

struct CodeBuffer {
    explicit CodeBuffer(u8* exec, u8* rw, u32 offset, size_t size)
            : exec_data(exec), rw_data(rw), offset(offset), size(size) {}

    inline void Flush() const {
        ClearDCache(rw_data, size);
        ClearDCache(exec_data, size);
        ClearICache(exec_data, size);
    }

    u8* exec_data;
    u8* rw_data;
    u32 offset;
    size_t size;
};

class CodeCache {
public:
    explicit CodeCache(const Config& config, u32 size, bool read_only = false);

    ~CodeCache();

    [[nodiscard]] std::optional<CodeBuffer> AllocCode(size_t size);
    bool FreeCode(u8* exec_ptr);
    [[nodiscard]] bool Contain(const u8* exec_ptr);
    [[nodiscard]] u8* GetExePtr(u32 offset);
    [[nodiscard]] u8* GetRWPtr(u32 offset);
    [[nodiscard]] u8* GetRWPtr(const u8* exec_ptr);
    [[nodiscard]] const CodeRegion& GetRegion() const { return region; }

    // Initialize the shared cold trampoline before registering a site.
    [[nodiscard]] bool InitializeRegionTrampoline(LinkManager& manager,
                                                  void* return_host,
                                                  void* dispatcher);
    [[nodiscard]] void* GetRegionTrampoline() const;
    [[nodiscard]] arm64::RegionLinkContext* GetRegionLinkContext() const {
        return region_link_context_.get();
    }

private:
    void Init();

    const Config& config;
    const size_t inst_alignment;
    const bool read_only;
    u32 max_size;
    mspace space_code{};
    std::unique_ptr<MemMap> code_mem;

    u8* code_mem_mapped{};
    u8* code_cursor{};
    CodeRegion region{};
    std::unique_ptr<arm64::RegionLinkContext> region_link_context_{};
};

}  // namespace swift::runtime::backend
