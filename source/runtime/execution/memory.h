#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace swift::runtime::execution {

enum class BackingKind : std::uint32_t { Anonymous, FilePrivate, FileShared, Other };
struct MemoryRegion {
    std::uint64_t address;
    std::size_t size;
    int protection;
    BackingKind backing{BackingKind::Anonymous};
    std::uint64_t device{}, inode{}, file_offset{}, file_size{};
};
static_assert(sizeof(MemoryRegion) == 56);
struct ElfImage {
    std::uint64_t entry{}, phdr{}, load_bias{};
    std::uint16_t phnum{}, phentsize{};
};

// The one directory/owner of guest==host mappings, including reserved holes,
// stack guards and brk reserve. Native code lives in its separate arena.
class GuestMemory {
public:
    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    ElfImage LoadELF(const std::string&);
    std::uint64_t InitialStack(const ElfImage&, std::span<const std::string> argv,
                               std::span<const std::string> environment);
    std::uint64_t Map(std::uint64_t address, std::size_t length, int protection,
                      int flags, int fd = -1, std::uint64_t offset = 0);
    int Unmap(std::uint64_t address, std::size_t length);
    int Protect(std::uint64_t address, std::size_t length, int protection);
    std::uint64_t Brk(std::uint64_t requested);
    bool Contains(std::uint64_t address, std::size_t length, int protection) const noexcept;
    bool CopyOut(std::uint64_t address, const void*, std::size_t) const noexcept;
    std::span<const MemoryRegion> Regions() const noexcept { return regions; }
    std::span<const std::uint8_t> CodeAt(std::uint64_t pc) const noexcept;
    std::size_t PageSize() const noexcept { return page_size; }
    // Mutability is conservative over the containing mapping region, keeping
    // entry and Function-window decisions consistent without a per-code table.
    bool CodeMutable(std::uint64_t pc) const noexcept;
    bool WriteAffectsCode(std::uint64_t address, std::size_t length) const noexcept;
    // After a successful file mutator, refresh actual bounds once and return
    // whether executable observers require retirement before guest resumes.
    bool RefreshFileMappings(int fd);
    // Actual writable source/alias pages acquire a record. Regions
    // retain guest permissions while the host mapping temporarily excludes W.
    // Arm allocates/records before protecting; recovery is signal-safe and
    // never allocates. Clear restores current mappings, not saved permissions.
    void ArmCodeWrites(std::uint64_t address, std::size_t length);
    bool RecoverCodeWrite(std::uintptr_t address) noexcept;
    void ClearCodeWrites();

private:
    struct CodeWritePage { std::uint64_t address; bool armed; };
    std::vector<MemoryRegion> regions;
    std::vector<CodeWritePage> code_write_pages;
    std::size_t page_size;
    std::size_t file_regions{}, executable_file_regions{};
    std::uint64_t brk_start{}, brk_current{}, brk_limit{};
    std::vector<MemoryRegion> Replaced(std::uint64_t address, std::size_t length, int protection, bool remove,
                                       const MemoryRegion* replacement = nullptr) const;
    void CommitRegions(std::vector<MemoryRegion>&) noexcept;
    int PageProtection(std::uint64_t address) const noexcept;
};

} // namespace swift::runtime::execution
