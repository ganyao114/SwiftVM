#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "mem_map.h"

namespace swift::runtime::execution {

enum class BackingKind : std::uint32_t { Anonymous, FilePrivate, FileShared, Other };
enum class SourceMode : std::uint8_t { Immutable, WatchedBlock, Volatile };
struct MemoryRegion {
    std::uint64_t address;
    std::size_t size;
    int protection;
    BackingKind backing{BackingKind::Anonymous};
    std::uint64_t device{}, inode{}, file_offset{}, file_size{};
};
static_assert(sizeof(MemoryRegion) == 56);

// The one directory/owner of guest==host mappings, including reserved holes,
// stack guards and brk reserve. Native code lives in its separate arena.
class GuestMemory {
public:
    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    void InitializeBrk(std::uint64_t start, std::uint64_t limit) noexcept {
        brk_start = brk_current = start;
        brk_limit = limit;
    }
    std::uint64_t Map(std::uint64_t address,
                      std::size_t length,
                      int protection,
                      int flags,
                      int fd = -1,
                      std::uint64_t offset = 0);
    int Unmap(std::uint64_t address, std::size_t length);
    int Protect(std::uint64_t address, std::size_t length, int protection);
    std::uint64_t Brk(std::uint64_t requested);
    bool Contains(std::uint64_t address, std::size_t length, int protection) const noexcept;
    bool Accessible(std::uint64_t address, std::size_t length, int protection) const noexcept;
    bool CopyIn(void*, std::uint64_t address, std::size_t) const noexcept;
    bool CopyOut(std::uint64_t address, const void*, std::size_t) const noexcept;
    MappingRelease Release() noexcept;
    std::span<const MemoryRegion> Regions() const noexcept { return regions; }
    std::span<const std::uint8_t> CodeAt(std::uint64_t pc) const noexcept;
    std::size_t PageSize() const noexcept { return page_size; }
    // One admission decision from the actual region ledger. Missing means no
    // readable/executable mapping. Device/unknown backing is Volatile and has
    // neither persistent-JIT nor direct-fetch qualification in this service.
    // Regular files/anonymous sources retain the current declared external-
    // writer boundary; writable sources/shared writers require WatchedBlock.
    std::optional<SourceMode> ClassifySource(std::uint64_t pc) const noexcept;
    bool WriteAffectsCode(std::uint64_t address, std::size_t length) const noexcept;
    // After a successful file mutator, refresh actual bounds once and return
    // whether executable observers require retirement before guest resumes.
    bool RefreshFileMappings(int fd);
    // Read by SMC's armed-page owner; never restores an earlier mapping's flags.
    int PageProtection(std::uint64_t address) const noexcept;

private:
    std::vector<MemoryRegion> regions;
    std::size_t page_size;
    std::size_t file_regions{}, executable_file_regions{};
    std::uint64_t brk_start{}, brk_current{}, brk_limit{};
    std::vector<MemoryRegion> Replaced(std::uint64_t address,
                                       std::size_t length,
                                       int protection,
                                       bool remove,
                                       const MemoryRegion* replacement = nullptr) const;
    void CommitRegions(std::vector<MemoryRegion>&) noexcept;
};

}  // namespace swift::runtime::execution
