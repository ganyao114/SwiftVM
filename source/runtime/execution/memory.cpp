#include "memory.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace swift::runtime::execution {
namespace {
using u64 = std::uint64_t;
constexpr u64 failed = UINT64_MAX;
[[noreturn]] void Error(const std::string& message) { throw std::runtime_error(message); }
[[noreturn]] void SystemError(const char* operation) { Error(std::string(operation) + ": " + std::strerror(errno)); }
u64 Add(u64 left, u64 right) {
    if (right > UINT64_MAX - left) Error("guest address/size overflow");
    return left + right;
}
u64 Up(u64 value, u64 page) { return Add(value, page - 1) & ~(page - 1); }
bool IsFile(const MemoryRegion& region) noexcept {
    return region.backing == BackingKind::FilePrivate || region.backing == BackingKind::FileShared;
}
bool SameFile(const MemoryRegion& left, const MemoryRegion& right) noexcept {
    return IsFile(left) && IsFile(right) && left.device == right.device && left.inode == right.inode;
}
bool FileOverlap(const MemoryRegion& source, const MemoryRegion& peer, u64 first, u64 last) noexcept {
    return SameFile(source, peer) && first < peer.file_offset + peer.size && peer.file_offset < last;
}
const MemoryRegion* RegionAt(std::span<const MemoryRegion> regions, u64 address) noexcept {
    const auto next = std::upper_bound(regions.begin(), regions.end(), address,
            [](u64 value, const MemoryRegion& region) { return value < region.address; });
    if (next == regions.begin()) return nullptr;
    const auto& region = *std::prev(next);
    return address - region.address < region.size ? &region : nullptr;
}
MemoryRegion Slice(const MemoryRegion& region, u64 first, u64 last) noexcept {
    auto result = region;
    result.address = first; result.size = static_cast<std::size_t>(last - first);
    if (region.backing != BackingKind::Anonymous) result.file_offset += first - region.address;
    return result;
}
bool Coalescible(const MemoryRegion& left, const MemoryRegion& right) noexcept {
    return left.address + left.size == right.address && left.protection == right.protection && left.backing == right.backing &&
            (left.backing == BackingKind::Anonymous ||
             (SameFile(left, right) && left.file_offset + left.size == right.file_offset && left.file_size == right.file_size));
}
template<typename Visit>
void CodeWriteRanges(std::span<const MemoryRegion> regions, u64 address, u64 end, Visit visit) {
    for (const auto& source : regions) {
        const auto begin = std::max(address, source.address), tail = std::min(end, source.address + source.size);
        if (begin >= tail || !(source.protection & PROT_EXEC)) continue;
        if (source.protection & PROT_WRITE) visit(begin, tail);
        if (!IsFile(source)) continue;
        const auto first = source.file_offset + (begin - source.address), last = source.file_offset + (tail - source.address);
        for (const auto& peer : regions) {
            if (&source == &peer || peer.backing != BackingKind::FileShared || !(peer.protection & PROT_WRITE) ||
                !FileOverlap(source, peer, first, last)) continue;
            visit(peer.address + (std::max(first, peer.file_offset) - peer.file_offset),
                  peer.address + (std::min(last, peer.file_offset + peer.size) - peer.file_offset));
        }
    }
}
void ReadAt(int fd, void* destination, std::size_t size, u64 offset) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (size) {
        const auto amount = pread(fd, bytes, std::min(size, std::size_t{1} << 20), static_cast<off_t>(offset));
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) Error("truncated/unreadable ELF file");
        bytes += amount; size -= static_cast<std::size_t>(amount); offset += static_cast<u64>(amount);
    }
}
}

GuestMemory::GuestMemory() {
    const auto page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (page & (page - 1))) Error("unsupported host page size");
    page_size = static_cast<std::size_t>(page);
}
GuestMemory::~GuestMemory() {
    for (const auto& region : regions) munmap(reinterpret_cast<void*>(region.address), region.size);
}
bool GuestMemory::Contains(u64 address, std::size_t size, int protection) const noexcept {
    if (!size) return true;
    if (size > UINT64_MAX - address) return false;
    const auto end = address + size;
    for (const auto& region : regions) {
        if (region.address + region.size <= address) continue;
        if (region.address > address || (region.protection & protection) != protection) return false;
        address = std::min(end, region.address + region.size);
        if (address == end) return true;
    }
    return size == 0;
}
bool GuestMemory::CopyOut(u64 address, const void* source, std::size_t size) const noexcept {
    if (!Contains(address, size, PROT_WRITE)) return false;
    if (size) std::memcpy(reinterpret_cast<void*>(address), source, size);
    return true;
}
std::span<const std::uint8_t> GuestMemory::CodeAt(u64 pc) const noexcept {
    const auto* region = RegionAt(regions, pc);
    if (!region || (region->protection & (PROT_READ | PROT_EXEC)) != (PROT_READ | PROT_EXEC)) return {};
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
    return offset < readable ? std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(pc), readable - offset}
                             : std::span<const std::uint8_t>{};
}
int GuestMemory::PageProtection(u64 address) const noexcept {
    const auto* region = RegionAt(regions, address);
    return region && region->size >= page_size && address - region->address <= region->size - page_size
            ? region->protection : -1;
}
bool GuestMemory::CodeMutable(u64 pc) const noexcept {
    const auto* source = RegionAt(regions, pc);
    if (!source || !(source->protection & PROT_EXEC)) return false;
    if (source->protection & PROT_WRITE) return true;
    if (!IsFile(*source)) return false;
    for (const auto& peer : regions)
        if (peer.backing == BackingKind::FileShared && (peer.protection & PROT_WRITE) &&
            FileOverlap(*source, peer, source->file_offset, source->file_offset + source->size)) return true;
    return false;
}
bool GuestMemory::WriteAffectsCode(u64 address, std::size_t length) const noexcept {
    if (!length) return false;
    if (length > UINT64_MAX - address) return true;
    const auto end = address + length;
    for (const auto& written : regions) {
        const auto begin = std::max(address, written.address), tail = std::min(end, written.address + written.size);
        if (begin >= tail) continue;
        if (written.protection & PROT_EXEC) return true;
        if (!executable_file_regions || written.backing != BackingKind::FileShared) continue;
        const auto first = written.file_offset + (begin - written.address), last = written.file_offset + (tail - written.address);
        for (const auto& source : regions)
            if ((source.protection & PROT_EXEC) && FileOverlap(written, source, first, last)) return true;
    }
    return false;
}
bool GuestMemory::RefreshFileMappings(int fd) {
    if (!file_regions) return false; // Anonymous ELF/stdout needs no fstat.
    struct stat status{};
    if (fstat(fd, &status)) SystemError("refresh guest file mapping");
    if (!S_ISREG(status.st_mode)) return false;
    if (status.st_size < 0) Error("negative guest file size");
    bool executable{};
    for (auto& region : regions)
        if (IsFile(region) && region.device == static_cast<u64>(status.st_dev) && region.inode == static_cast<u64>(status.st_ino)) {
            region.file_size = static_cast<u64>(status.st_size);
            executable |= (region.protection & PROT_EXEC) != 0;
        }
    return executable;
}
void GuestMemory::ArmCodeWrites(u64 address, std::size_t length) {
    if (!length) return;
    const auto end = Add(address, length);
    std::size_t candidate_pages{};
    CodeWriteRanges(regions, address, end, [&](u64 begin, u64 tail) {
        const auto count = ((tail - 1) / page_size) - (begin / page_size) + 1;
        if (count > code_write_pages.max_size() - code_write_pages.size() - candidate_pages)
            throw std::length_error("guest code-write page capacity overflow");
        candidate_pages += static_cast<std::size_t>(count);
    });
    if (!candidate_pages) return;
    code_write_pages.reserve(code_write_pages.size() + candidate_pages);
    CodeWriteRanges(regions, address, end, [&](u64 begin, u64 tail) {
        const auto last = (tail - 1) & ~(u64{page_size} - 1);
        for (auto page = begin & ~(u64{page_size} - 1);; page += page_size) {
            code_write_pages.push_back({page, false});
            if (page == last) break;
        }
    });
    std::sort(code_write_pages.begin(), code_write_pages.end(),
            [](const CodeWritePage& left, const CodeWritePage& right) { return left.address < right.address; });
    std::size_t kept{};
    for (const auto page : code_write_pages) {
        if (kept && code_write_pages[kept - 1].address == page.address)
            code_write_pages[kept - 1].armed |= page.armed;
        else code_write_pages[kept++] = page;
    }
    code_write_pages.resize(kept);
    // All records and allocations precede protection, including non-EXEC
    // MAP_SHARED writers of these source pages. Keep existing armed records.
    for (auto& page : code_write_pages) {
        const auto protection = PageProtection(page.address);
        if (page.armed || protection < 0 || !(protection & PROT_WRITE)) continue;
        if (mprotect(reinterpret_cast<void*>(page.address), page_size, protection & ~PROT_WRITE)) SystemError("protect guest code source/alias");
        page.armed = true;
    }
}
bool GuestMemory::RecoverCodeWrite(std::uintptr_t address) noexcept {
    const auto page = address & ~(u64{page_size} - 1);
    const auto found = std::lower_bound(code_write_pages.begin(), code_write_pages.end(), page,
            [](const CodeWritePage& row, u64 value) { return row.address < value; });
    if (found == code_write_pages.end() || found->address != page || !found->armed) return false;
    const auto protection = PageProtection(page);
    if (protection < 0 || !(protection & PROT_WRITE)) return false;
    if (syscall(SYS_mprotect, reinterpret_cast<void*>(page), page_size, protection)) return false;
    found->armed = false;
    return true;
}
void GuestMemory::ClearCodeWrites() {
    for (auto& page : code_write_pages) if (page.armed) {
        const auto protection = PageProtection(page.address);
        // Mapping/protection syscalls may already have replaced or unmapped
        // this page. Never restore the earlier mapping's permissions.
        if (protection >= 0 && mprotect(reinterpret_cast<void*>(page.address), page_size, protection))
            SystemError("restore guest code source");
        page.armed = false;
    }
    code_write_pages.clear(); // Retain actual page-record capacity.
}
std::vector<MemoryRegion> GuestMemory::Replaced(u64 address, std::size_t size, int protection, bool remove,
                                               const MemoryRegion* replacement) const {
    const auto end = Add(address, size);
    std::vector<MemoryRegion> next; next.reserve(regions.size() + 3);
    for (const auto& region : regions) {
        const auto tail = region.address + region.size;
        if (tail <= address || region.address >= end) { next.push_back(region); continue; }
        if (region.address < address) next.push_back(Slice(region, region.address, address));
        if (!remove && !replacement) {
            auto middle = Slice(region, std::max(address, region.address), std::min(end, tail));
            middle.protection = protection; next.push_back(middle);
        }
        if (tail > end) next.push_back(Slice(region, end, tail));
    }
    if (replacement) next.push_back(*replacement);
    std::sort(next.begin(), next.end(), [](const auto& left, const auto& right) { return left.address < right.address; });
    std::size_t kept{};
    for (const auto& region : next) {
        if (kept && Coalescible(next[kept - 1], region))
            next[kept - 1].size += region.size;
        else next[kept++] = region;
    }
    next.resize(kept); return next;
}
void GuestMemory::CommitRegions(std::vector<MemoryRegion>& next) noexcept {
    regions.swap(next);
    file_regions = executable_file_regions = 0;
    for (const auto& region : regions) if (IsFile(region)) {
        ++file_regions;
        executable_file_regions += (region.protection & PROT_EXEC) != 0;
    }
}

u64 GuestMemory::Map(u64 address, std::size_t length, int protection, int flags, int fd, u64 offset) {
    if (!length || length > SIZE_MAX - page_size || offset > static_cast<u64>(std::numeric_limits<off_t>::max())) { errno = EINVAL; return failed; }
    const auto size = static_cast<std::size_t>(Up(length, page_size));
    if (size > UINT64_MAX - offset) { errno = EINVAL; return failed; }
    MemoryRegion mapping{address, size, protection};
    if (!(flags & MAP_ANONYMOUS)) {
        struct stat status{};
        if (fstat(fd, &status)) return failed;
        mapping.backing = S_ISREG(status.st_mode)
                ? ((flags & MAP_SHARED) ? BackingKind::FileShared : BackingKind::FilePrivate) : BackingKind::Other;
        mapping.device = static_cast<u64>(status.st_dev); mapping.inode = static_cast<u64>(status.st_ino);
        mapping.file_offset = offset;
        if (S_ISREG(status.st_mode)) {
            if (status.st_size < 0) { errno = EINVAL; return failed; }
            mapping.file_size = static_cast<u64>(status.st_size);
        }
    }
    const bool fixed = (flags & MAP_FIXED) != 0;
    if ((fixed && (address & (page_size - 1))) || size > UINT64_MAX - address) { errno = EINVAL; return failed; }
    const bool owned = fixed && Contains(address, size, 0);
    if (fixed && !owned) {
        for (const auto& region : regions)
            if (region.address < address + size && address < region.address + region.size) { errno = EINVAL; return failed; }
    }
    std::vector<MemoryRegion> next;
    if (owned) next = Replaced(address, size, protection, false, &mapping);
    void* mapped;
    if (fixed && !owned) {
#if defined(MAP_FIXED_NOREPLACE)
        mapped = mmap(reinterpret_cast<void*>(address), size, protection,
                (flags & ~MAP_FIXED) | MAP_FIXED_NOREPLACE, fd, static_cast<off_t>(offset));
        if (mapped == MAP_FAILED && errno == EINVAL)
#endif
            mapped = mmap(reinterpret_cast<void*>(address), size, protection, flags & ~MAP_FIXED, fd, static_cast<off_t>(offset));
        if (mapped != MAP_FAILED && reinterpret_cast<u64>(mapped) != address) { munmap(mapped, size); errno = EEXIST; return failed; }
    } else mapped = mmap(reinterpret_cast<void*>(address), size, protection, flags, fd, static_cast<off_t>(offset));
    if (mapped == MAP_FAILED) return failed;
    const auto base = reinterpret_cast<u64>(mapped);
    mapping.address = base;
    try { if (!owned) next = Replaced(base, size, protection, false, &mapping); }
    catch (...) { munmap(mapped, size); throw; }
    CommitRegions(next); return base;
}
int GuestMemory::Unmap(u64 address, std::size_t length) {
    if (!length || (address & (page_size - 1)) || length > SIZE_MAX - page_size) { errno = EINVAL; return -1; }
    const auto size = static_cast<std::size_t>(Up(length, page_size));
    if (size > UINT64_MAX - address) { errno = EINVAL; return -1; }
    auto next = Replaced(address, size, 0, true);
    // Never pass an unowned host range to MAP_FIXED/munmap. Guest-unmapped
    // holes remain a no-op even when the host has unrelated mappings there.
    for (const auto& region : regions) {
        const auto begin = std::max(address, region.address), end = std::min(address + size, region.address + region.size);
        if (begin < end && munmap(reinterpret_cast<void*>(begin), static_cast<std::size_t>(end - begin))) SystemError("guest munmap");
    }
    CommitRegions(next); return 0;
}
int GuestMemory::Protect(u64 address, std::size_t length, int protection) {
    if ((address & (page_size - 1)) || length > SIZE_MAX - page_size) { errno = EINVAL; return -1; }
    if (!length) return 0;
    const auto size = static_cast<std::size_t>(Up(length, page_size));
    if (!Contains(address, size, 0)) { errno = ENOMEM; return -1; }
    auto next = Replaced(address, size, protection, false);
    if (mprotect(reinterpret_cast<void*>(address), size, protection)) return -1;
    CommitRegions(next); return 0;
}
u64 GuestMemory::Brk(u64 requested) {
    if (!requested) return brk_current;
    if (requested < brk_start || requested > brk_limit) return brk_current;
    const auto old = Up(brk_current, page_size), next = Up(requested, page_size);
    if (next > old && Protect(old, static_cast<std::size_t>(next - old), PROT_READ | PROT_WRITE)) return brk_current;
    if (next < old) {
        if (Protect(next, static_cast<std::size_t>(old - next), PROT_NONE)) return brk_current;
        madvise(reinterpret_cast<void*>(next), static_cast<std::size_t>(old - next), MADV_DONTNEED);
    }
    brk_current = requested; return requested;
}

ElfImage GuestMemory::LoadELF(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) SystemError("open guest ELF");
    struct File { int fd; ~File() { close(fd); } } file{fd};
    struct stat status{};
    if (fstat(fd, &status) || status.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) Error("invalid ELF file size");
    const auto file_size = static_cast<u64>(status.st_size);
    Elf64_Ehdr header{}; ReadAt(fd, &header, sizeof(header), 0);
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT || header.e_machine != EM_X86_64 ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN) || header.e_ehsize < sizeof(header) ||
        header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum || header.e_phnum > 1024)
        Error("expected a little-endian x86-64 ET_EXEC/ET_DYN ELF");
    const auto table_size = std::size_t{header.e_phnum} * sizeof(Elf64_Phdr);
    if (header.e_phoff > file_size || table_size > file_size - header.e_phoff) Error("ELF program headers exceed file");
    std::vector<Elf64_Phdr> headers(header.e_phnum); ReadAt(fd, headers.data(), table_size, header.e_phoff);
    u64 low = UINT64_MAX, high{}, alignment = page_size;
    std::vector<Elf64_Phdr> loads;
    for (const auto& segment : headers) {
        if (segment.p_type == PT_INTERP) Error("PT_INTERP is not supported by this static ELF runner");
        if (segment.p_type != PT_LOAD || !segment.p_memsz) continue;
        if (segment.p_filesz > segment.p_memsz || segment.p_offset > file_size || segment.p_filesz > file_size - segment.p_offset)
            Error("ELF load segment exceeds file/memory bounds");
        if (segment.p_align > 1 && ((segment.p_align & (segment.p_align - 1)) ||
            ((segment.p_vaddr - segment.p_offset) & (segment.p_align - 1)))) Error("invalid ELF segment alignment");
        low = std::min<u64>(low, segment.p_vaddr & ~u64{page_size - 1});
        high = std::max(high, Up(Add(segment.p_vaddr, segment.p_memsz), page_size));
        alignment = std::max<u64>(alignment, segment.p_align); loads.push_back(segment);
    }
    if (loads.empty()) Error("ELF contains no loadable memory");
    std::sort(loads.begin(), loads.end(), [](const auto& a, const auto& b) { return a.p_vaddr < b.p_vaddr; });
    for (std::size_t i = 1; i < loads.size(); ++i)
        if (Add(loads[i - 1].p_vaddr, loads[i - 1].p_memsz) > loads[i].p_vaddr) Error("overlapping ELF segment bytes are unsupported");
    constexpr u64 heap_reserve = u64{256} << 20;
    const auto span = high - low, total = Add(span, heap_reserve);
    const auto reservation_size = header.e_type == ET_DYN ? Add(total, alignment) : total;
    if (reservation_size > SIZE_MAX) Error("ELF image span exceeds host size");
    const auto reserved = Map(header.e_type == ET_EXEC ? low : 0, static_cast<std::size_t>(reservation_size), PROT_NONE,
            MAP_PRIVATE | MAP_ANONYMOUS | (header.e_type == ET_EXEC ? MAP_FIXED : 0));
    if (reserved == failed) SystemError("reserve guest ELF");
    const auto delta = header.e_type == ET_DYN ? ((low - reserved) & (alignment - 1)) : 0;
    const auto base = Add(reserved, delta);
    if (delta) Unmap(reserved, static_cast<std::size_t>(delta));
    if (reservation_size > delta + total) Unmap(base + total, static_cast<std::size_t>(reservation_size - delta - total));
    const auto actual = [&](u64 address) {
        if (address < low || address >= high) Error("ELF address lies outside its image");
        return Add(base, address - low);
    };
    ElfImage image{actual(header.e_entry), 0, base - low, header.e_phnum, header.e_phentsize};
    std::vector<u64> boundaries{base, base + span};
    for (const auto& segment : loads) {
        const auto begin = actual(segment.p_vaddr) & ~u64{page_size - 1};
        const auto end = Up(Add(actual(segment.p_vaddr), segment.p_memsz), page_size);
        if (Protect(begin, static_cast<std::size_t>(end - begin), PROT_READ | PROT_WRITE)) SystemError("populate guest ELF");
        ReadAt(fd, reinterpret_cast<void*>(actual(segment.p_vaddr)), static_cast<std::size_t>(segment.p_filesz), segment.p_offset);
        // Anonymous pages start zero and actual segment byte extents do not overlap.
        boundaries.push_back(begin); boundaries.push_back(end);
        if (header.e_phoff >= segment.p_offset && header.e_phoff - segment.p_offset <= segment.p_filesz &&
            table_size <= segment.p_filesz - (header.e_phoff - segment.p_offset))
            image.phdr = actual(segment.p_vaddr) + header.e_phoff - segment.p_offset;
    }
    if (!image.phdr) Error("ELF program headers must be covered by PT_LOAD");
    std::sort(boundaries.begin(), boundaries.end()); boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (std::size_t i = 1; i < boundaries.size(); ++i) {
        int protection{};
        for (const auto& segment : loads) {
            const auto begin = actual(segment.p_vaddr) & ~u64{page_size - 1};
            const auto end = Up(Add(actual(segment.p_vaddr), segment.p_memsz), page_size);
            if (begin <= boundaries[i - 1] && boundaries[i] <= end) {
                if (segment.p_flags & PF_R) protection |= PROT_READ;
                if (segment.p_flags & PF_W) protection |= PROT_WRITE;
                if (segment.p_flags & PF_X) protection |= PROT_READ | PROT_EXEC;
            }
        }
        if (Protect(boundaries[i - 1], static_cast<std::size_t>(boundaries[i] - boundaries[i - 1]), protection)) SystemError("protect guest ELF");
    }
    if (CodeAt(image.entry).empty()) Error("ELF entry is not executable/readable");
    brk_start = brk_current = base + span; brk_limit = base + total;
    return image;
}

u64 GuestMemory::InitialStack(const ElfImage& image, std::span<const std::string> argv,
                               std::span<const std::string> environment) {
    constexpr std::size_t stack_size = 8u << 20;
    const auto base = Map(0, stack_size + page_size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (base == failed || Protect(base + page_size, stack_size, PROT_READ | PROT_WRITE)) SystemError("guest stack");
    const auto bottom = base + page_size; u64 top = bottom + stack_size;
    const auto push = [&](const void* data, std::size_t size) {
        if (size > top - bottom) Error("guest arguments exceed initial stack");
        top -= size; std::memcpy(reinterpret_cast<void*>(top), data, size); return top;
    };
    const char platform[] = "x86_64"; const auto platform_at = push(platform, sizeof(platform));
    std::uint8_t random[16];
    const auto fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) SystemError("open AT_RANDOM source");
    std::size_t received{};
    while (received < sizeof(random)) {
        const auto count = read(fd, random + received, sizeof(random) - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); Error("read AT_RANDOM source"); }
        received += static_cast<std::size_t>(count);
    }
    close(fd); const auto random_at = push(random, sizeof(random));
    std::vector<u64> arguments, variables;
    for (const auto& argument : argv) arguments.push_back(push(argument.c_str(), argument.size() + 1));
    for (const auto& variable : environment) variables.push_back(push(variable.c_str(), variable.size() + 1));
    std::vector<u64> words{argv.size()}; words.insert(words.end(), arguments.begin(), arguments.end()); words.push_back(0);
    words.insert(words.end(), variables.begin(), variables.end()); words.push_back(0);
    const auto aux = [&](u64 key, u64 value) { words.push_back(key); words.push_back(value); };
    aux(AT_PHDR, image.phdr); aux(AT_PHENT, image.phentsize); aux(AT_PHNUM, image.phnum); aux(AT_PAGESZ, page_size);
    aux(AT_BASE, 0); aux(AT_FLAGS, 0); aux(AT_ENTRY, image.entry); aux(AT_UID, getuid()); aux(AT_EUID, geteuid());
    aux(AT_GID, getgid()); aux(AT_EGID, getegid()); aux(AT_SECURE, 0); aux(AT_RANDOM, random_at); aux(AT_PLATFORM, platform_at);
    aux(AT_HWCAP, 0); aux(AT_HWCAP2, 0); aux(AT_CLKTCK, static_cast<u64>(std::max(1L, sysconf(_SC_CLK_TCK))));
    aux(AT_EXECFN, arguments.empty() ? 0 : arguments.front()); aux(AT_NULL, 0);
    if (words.size() > (top - bottom) / 8) Error("guest stack table exceeds stack");
    top = (top - words.size() * 8) & ~u64{15};
    if (top < bottom) Error("guest stack alignment exceeds stack");
    std::memcpy(reinterpret_cast<void*>(top), words.data(), words.size() * 8); return top;
}

} // namespace swift::runtime::execution
