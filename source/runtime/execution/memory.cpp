#include "memory.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "base/error.h"

#include "runtime/platform/linux/safe_access.h"

namespace swift::runtime::execution {
namespace {
using u64 = std::uint64_t;
constexpr u64 failed = UINT64_MAX;

[[noreturn]] void Error(const std::string& message) { swift::ThrowError(message); }

[[noreturn]] void SystemError(const char* operation) {
    Error(std::string(operation) + ": " + std::strerror(errno));
}

u64 Add(u64 left, u64 right) {
    if (right > UINT64_MAX - left) Error("guest address/size overflow");
    return left + right;
}

u64 Up(u64 value, u64 page) { return Add(value, page - 1) & ~(page - 1); }

bool IsFile(const MemoryRegion& region) noexcept {
    return region.backing == BackingKind::FilePrivate || region.backing == BackingKind::FileShared;
}

bool SameFile(const MemoryRegion& left, const MemoryRegion& right) noexcept {
    return IsFile(left) && IsFile(right) && left.device == right.device &&
           left.inode == right.inode;
}

bool FileOverlap(const MemoryRegion& source,
                 const MemoryRegion& peer,
                 u64 first,
                 u64 last) noexcept {
    return SameFile(source, peer) && first < peer.file_offset + peer.size &&
           peer.file_offset < last;
}

const MemoryRegion* RegionAt(std::span<const MemoryRegion> regions, u64 address) noexcept {
    const auto next = std::upper_bound(
            regions.begin(), regions.end(), address, [](u64 value, const MemoryRegion& region) {
                return value < region.address;
            });
    if (next == regions.begin()) return nullptr;
    const auto& region = *std::prev(next);
    return address - region.address < region.size ? &region : nullptr;
}

MemoryRegion Slice(const MemoryRegion& region, u64 first, u64 last) noexcept {
    auto result = region;
    result.address = first;
    result.size = static_cast<std::size_t>(last - first);
    if (IsFile(region)) result.file_offset += first - region.address;
    return result;
}

bool Coalescible(const MemoryRegion& left, const MemoryRegion& right) noexcept {
    return left.address + left.size == right.address && left.protection == right.protection &&
           left.backing == right.backing &&
           (left.backing == BackingKind::Anonymous ||
            left.backing == BackingKind::AnonymousShared ||
            (SameFile(left, right) && left.file_offset + left.size == right.file_offset &&
             left.file_size == right.file_size));
}

}  // namespace

GuestMemory::GuestMemory() {
    const auto page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (page & (page - 1))) Error("unsupported host page size");
    page_size = static_cast<std::size_t>(page);
}

GuestMemory::~GuestMemory() { (void)Release(); }

MappingRelease GuestMemory::Release() noexcept {
    MappingRelease result;
    std::size_t kept{};
    for (const auto& region : regions) {
        if (munmap(reinterpret_cast<void*>(region.address), region.size)) {
            if (!result.unmap_error) result.unmap_error = errno;
            regions[kept++] = region;
        }
    }
    regions.resize(kept);
    result.remaining_views = kept;
    file_regions = executable_file_regions = 0;
    for (const auto& region : regions)
        if (IsFile(region)) {
            ++file_regions;
            executable_file_regions += (region.protection & PROT_EXEC) != 0;
        }
    return result;
}

bool GuestMemory::Contains(u64 address, std::size_t size, int protection) const noexcept {
    if (!size) return true;
    if (size > UINT64_MAX - address) return false;
    const auto end = address + size;
    for (const auto& region : regions) {
        if (region.address + region.size <= address) continue;
        if (region.address > address || (region.protection & protection) != protection)
            return false;
        address = std::min(end, region.address + region.size);
        if (address == end) return true;
    }
    return size == 0;
}

bool GuestMemory::Accessible(u64 address, std::size_t size, int protection) const noexcept {
    if (!Contains(address, size, protection)) return false;
    if (!size) return true;
    const auto end = address + size;
    for (const auto& region : regions) {
        if (region.address >= end) break;
        if (region.address + region.size <= address || !IsFile(region)) continue;
        if (region.file_size <= region.file_offset) return false;
        const auto bytes = region.file_size - region.file_offset;
        const auto pages = (bytes - 1) / page_size + 1;
        const auto accessed = std::min(end, region.address + region.size) - region.address;
        if (pages <= UINT64_MAX / page_size && accessed > pages * page_size) return false;
    }
    return true;
}

bool GuestMemory::PrivateAnonymous(u64 address, std::size_t size) const noexcept {
    if (!size) return true;
    if (size > UINT64_MAX - address) return false;
    const auto end = address + size;
    for (const auto& region : regions) {
        if (region.address + region.size <= address) continue;
        if (region.address > address || region.backing != BackingKind::Anonymous) return false;
        address = std::min(end, region.address + region.size);
        if (address == end) return true;
    }
    return size == 0;
}

bool GuestMemory::CopyIn(void* destination, u64 address, std::size_t size) const noexcept {
    return Accessible(address, size, PROT_READ) &&
           platform::ReadGuestBytes(destination, address, size);
}

bool GuestMemory::CopyOut(u64 address, const void* source, std::size_t size) const noexcept {
    return Accessible(address, size, PROT_WRITE) &&
           platform::WriteGuestBytes(address, source, size);
}

std::span<const std::uint8_t> GuestMemory::CodeAt(u64 pc) const noexcept {
    const auto* region = RegionAt(regions, pc);
    if (!region || region->backing == BackingKind::Other ||
        (region->protection & (PROT_READ | PROT_EXEC)) != (PROT_READ | PROT_EXEC))
        return {};
    auto readable = region->size;
    if (IsFile(*region)) {
        if (region->file_size <= region->file_offset) return {};
        // Bytes after EOF within the last file page are readable zero-fill;
        // later pages fault in the host and must never enter the decoder span.
        const auto file_bytes = region->file_size - region->file_offset;
        const auto accessible = ((file_bytes - 1) / page_size + 1) * page_size;
        readable = static_cast<std::size_t>(std::min<u64>(readable, accessible));
    }
    const auto offset = static_cast<std::size_t>(pc - region->address);
    return offset < readable
                   ? std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(pc),
                                                   readable - offset}
                   : std::span<const std::uint8_t>{};
}

int GuestMemory::PageProtection(u64 address) const noexcept {
    const auto* region = RegionAt(regions, address);
    return region && region->size >= page_size &&
                           address - region->address <= region->size - page_size
                   ? region->protection
                   : -1;
}

std::optional<SourceMode> GuestMemory::ClassifySource(u64 pc) const noexcept {
    const auto* source = RegionAt(regions, pc);
    if (!source || (source->protection & (PROT_READ | PROT_EXEC)) != (PROT_READ | PROT_EXEC))
        return {};
    if (source->backing == BackingKind::Other) return SourceMode::Volatile;
    if (source->protection & PROT_WRITE) return SourceMode::WatchedBlock;
    if (!IsFile(*source)) return SourceMode::Immutable;
    for (const auto& peer : regions)
        if (peer.backing == BackingKind::FileShared && (peer.protection & PROT_WRITE) &&
            FileOverlap(*source, peer, source->file_offset, source->file_offset + source->size))
            return SourceMode::WatchedBlock;
    return SourceMode::Immutable;
}

bool GuestMemory::WriteAffectsCode(u64 address, std::size_t length) const noexcept {
    if (!length) return false;
    if (length > UINT64_MAX - address) return true;
    const auto end = address + length;
    for (const auto& written : regions) {
        const auto begin = std::max(address, written.address),
                   tail = std::min(end, written.address + written.size);
        if (begin >= tail) continue;
        if (written.protection & PROT_EXEC) return true;
        if (!executable_file_regions || written.backing != BackingKind::FileShared) continue;
        const auto first = written.file_offset + (begin - written.address),
                   last = written.file_offset + (tail - written.address);
        for (const auto& source : regions)
            if ((source.protection & PROT_EXEC) && FileOverlap(written, source, first, last))
                return true;
    }
    return false;
}

bool GuestMemory::RefreshFileMappings(int fd) {
    if (!file_regions) return false;  // Anonymous ELF/stdout needs no fstat.
    struct stat status{};
    if (fstat(fd, &status)) SystemError("refresh guest file mapping");
    if (!S_ISREG(status.st_mode)) return false;
    if (status.st_size < 0) Error("negative guest file size");
    bool executable{};
    for (auto& region : regions)
        if (IsFile(region) && region.device == static_cast<u64>(status.st_dev) &&
            region.inode == static_cast<u64>(status.st_ino)) {
            region.file_size = static_cast<u64>(status.st_size);
            executable |= (region.protection & PROT_EXEC) != 0;
        }
    return executable;
}

std::vector<MemoryRegion> GuestMemory::Replaced(u64 address,
                                                std::size_t size,
                                                int protection,
                                                bool remove,
                                                const MemoryRegion* replacement) const {
    const auto end = Add(address, size);
    std::vector<MemoryRegion> next;
    next.reserve(regions.size() + 3);
    for (const auto& region : regions) {
        const auto tail = region.address + region.size;
        if (tail <= address || region.address >= end) {
            next.push_back(region);
            continue;
        }
        if (region.address < address) next.push_back(Slice(region, region.address, address));
        if (!remove && !replacement) {
            auto middle = Slice(region, std::max(address, region.address), std::min(end, tail));
            middle.protection = protection;
            next.push_back(middle);
        }
        if (tail > end) next.push_back(Slice(region, end, tail));
    }
    if (replacement) next.push_back(*replacement);
    std::sort(next.begin(), next.end(), [](const auto& left, const auto& right) {
        return left.address < right.address;
    });
    std::size_t kept{};
    for (const auto& region : next) {
        if (kept && Coalescible(next[kept - 1], region))
            next[kept - 1].size += region.size;
        else
            next[kept++] = region;
    }
    next.resize(kept);
    return next;
}

void GuestMemory::CommitRegions(std::vector<MemoryRegion>& next) noexcept {
    regions.swap(next);
    file_regions = executable_file_regions = 0;
    for (const auto& region : regions)
        if (IsFile(region)) {
            ++file_regions;
            executable_file_regions += (region.protection & PROT_EXEC) != 0;
        }
}

u64 GuestMemory::Map(
        u64 address, std::size_t length, int protection, int flags, int fd, u64 offset) {
    if (!length || length > SIZE_MAX - page_size ||
        offset > static_cast<u64>(std::numeric_limits<off_t>::max())) {
        errno = EINVAL;
        return failed;
    }
    const auto size = static_cast<std::size_t>(Up(length, page_size));
    if (size > UINT64_MAX - offset) {
        errno = EINVAL;
        return failed;
    }
    MemoryRegion mapping{address, size, protection};
    if (flags & MAP_ANONYMOUS) {
        if (flags & MAP_SHARED) mapping.backing = BackingKind::AnonymousShared;
    } else {
        struct stat status{};
        if (fstat(fd, &status)) return failed;
        mapping.backing = S_ISREG(status.st_mode)
                                  ? ((flags & MAP_SHARED) ? BackingKind::FileShared
                                                          : BackingKind::FilePrivate)
                                  : BackingKind::Other;
        mapping.device = static_cast<u64>(status.st_dev);
        mapping.inode = static_cast<u64>(status.st_ino);
        mapping.file_offset = offset;
        if (S_ISREG(status.st_mode)) {
            if (status.st_size < 0) {
                errno = EINVAL;
                return failed;
            }
            mapping.file_size = static_cast<u64>(status.st_size);
        }
    }
    const bool fixed = (flags & MAP_FIXED) != 0;
    if ((fixed && (address & (page_size - 1))) || size > UINT64_MAX - address) {
        errno = EINVAL;
        return failed;
    }
    const bool owned = fixed && Contains(address, size, 0);
    if (fixed && !owned) {
        for (const auto& region : regions)
            if (region.address < address + size && address < region.address + region.size) {
                errno = EINVAL;
                return failed;
            }
    }
    std::vector<MemoryRegion> next;
    if (owned) next = Replaced(address, size, protection, false, &mapping);
    void* mapped;
    if (fixed && !owned) {
#if defined(MAP_FIXED_NOREPLACE)
        mapped = mmap(reinterpret_cast<void*>(address),
                      size,
                      protection,
                      (flags & ~MAP_FIXED) | MAP_FIXED_NOREPLACE,
                      fd,
                      static_cast<off_t>(offset));
        if (mapped == MAP_FAILED && errno == EINVAL)
#endif
            mapped = mmap(reinterpret_cast<void*>(address),
                          size,
                          protection,
                          flags & ~MAP_FIXED,
                          fd,
                          static_cast<off_t>(offset));
        if (mapped != MAP_FAILED && reinterpret_cast<u64>(mapped) != address) {
            munmap(mapped, size);
            errno = EEXIST;
            return failed;
        }
    } else
        mapped = mmap(reinterpret_cast<void*>(address),
                      size,
                      protection,
                      flags,
                      fd,
                      static_cast<off_t>(offset));
    if (mapped == MAP_FAILED) return failed;
    const auto base = reinterpret_cast<u64>(mapped);
    mapping.address = base;
    try {
        if (!owned) next = Replaced(base, size, protection, false, &mapping);
    } catch (...) {
        munmap(mapped, size);
        throw;
    }
    CommitRegions(next);
    return base;
}

int GuestMemory::Unmap(u64 address, std::size_t length) {
    if (!length || (address & (page_size - 1)) || length > SIZE_MAX - page_size) {
        errno = EINVAL;
        return -1;
    }
    const auto size = static_cast<std::size_t>(Up(length, page_size));
    if (size > UINT64_MAX - address) {
        errno = EINVAL;
        return -1;
    }
    auto next = Replaced(address, size, 0, true);
    // Never pass an unowned host range to MAP_FIXED/munmap. Guest-unmapped
    // holes remain a no-op even when the host has unrelated mappings there.
    for (const auto& region : regions) {
        const auto begin = std::max(address, region.address),
                   end = std::min(address + size, region.address + region.size);
        if (begin < end &&
            munmap(reinterpret_cast<void*>(begin), static_cast<std::size_t>(end - begin)))
            SystemError("guest munmap");
    }
    CommitRegions(next);
    return 0;
}

int GuestMemory::Protect(u64 address, std::size_t length, int protection) {
    if ((address & (page_size - 1)) || length > SIZE_MAX - page_size) {
        errno = EINVAL;
        return -1;
    }
    if (!length) return 0;
    const auto size = static_cast<std::size_t>(Up(length, page_size));
    if (!Contains(address, size, 0)) {
        errno = ENOMEM;
        return -1;
    }
    auto next = Replaced(address, size, protection, false);
    if (mprotect(reinterpret_cast<void*>(address), size, protection)) return -1;
    CommitRegions(next);
    return 0;
}

u64 GuestMemory::Brk(u64 requested) {
    if (!requested) return brk_current;
    if (requested < brk_start || requested > brk_limit) return brk_current;
    const auto old = Up(brk_current, page_size), next = Up(requested, page_size);
    if (next > old && Protect(old, static_cast<std::size_t>(next - old), PROT_READ | PROT_WRITE))
        return brk_current;
    if (next < old) {
        if (Protect(next, static_cast<std::size_t>(old - next), PROT_NONE)) return brk_current;
        madvise(reinterpret_cast<void*>(next), static_cast<std::size_t>(old - next), MADV_DONTNEED);
    }
    brk_current = requested;
    return requested;
}

}  // namespace swift::runtime::execution
