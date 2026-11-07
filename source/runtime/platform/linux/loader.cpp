#include "loader.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include "base/error.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace swift::runtime::platform {
namespace {
using u64 = std::uint64_t;
constexpr auto failed = UINT64_MAX;

[[noreturn]] void Error(const std::string& text) {
    swift::ThrowError(text, ErrorKind::InvalidArgument);
}

[[noreturn]] void SystemError(const char* op) { swift::ThrowSystemError(op, errno); }

u64 Add(u64 a, u64 b) {
    if (b > UINT64_MAX - a) Error("guest address/size overflow");
    return a + b;
}

u64 Up(u64 a, u64 p) { return Add(a, p - 1) & ~(p - 1); }

void ReadAt(int fd, void* destination, std::size_t size, u64 offset) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (size) {
        const auto amount =
                pread(fd, bytes, std::min(size, std::size_t{1} << 20), static_cast<off_t>(offset));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) SystemError("read guest ELF");
        if (!amount) Error("truncated ELF file");
        bytes += amount;
        size -= static_cast<std::size_t>(amount);
        offset += static_cast<u64>(amount);
    }
}
}  // namespace

LoadedImage LoadELF(execution::GuestMemory& memory, const std::string& path) {
    const auto page_size = memory.PageSize();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) SystemError("open guest ELF");

    struct File {
        int fd;

        ~File() { close(fd); }
    } file{fd};
    struct stat status{};
    if (fstat(fd, &status)) SystemError("stat guest ELF");
    if (status.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) Error("invalid ELF file size");
    const auto file_size = static_cast<u64>(status.st_size);
    Elf64_Ehdr header{};
    ReadAt(fd, &header, sizeof(header), 0);
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT || header.e_machine != EM_X86_64 ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN) || header.e_ehsize < sizeof(header) ||
        header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum || header.e_phnum > 1024)
        Error("expected a little-endian x86-64 ET_EXEC/ET_DYN ELF");
    const auto table_size = std::size_t{header.e_phnum} * sizeof(Elf64_Phdr);
    if (header.e_phoff > file_size || table_size > file_size - header.e_phoff)
        Error("ELF program headers exceed file");
    std::vector<Elf64_Phdr> headers(header.e_phnum);
    ReadAt(fd, headers.data(), table_size, header.e_phoff);
    u64 low = UINT64_MAX, high{}, alignment = page_size;
    std::vector<Elf64_Phdr> loads;
    for (const auto& segment : headers) {
        if (segment.p_type == PT_INTERP)
            Error("PT_INTERP is not supported by this static ELF runner");
        if (segment.p_type != PT_LOAD || !segment.p_memsz) continue;
        if (segment.p_filesz > segment.p_memsz || segment.p_offset > file_size ||
            segment.p_filesz > file_size - segment.p_offset)
            Error("ELF load segment exceeds file/memory bounds");
        if (segment.p_align > 1 && ((segment.p_align & (segment.p_align - 1)) ||
                                    ((segment.p_vaddr - segment.p_offset) & (segment.p_align - 1))))
            Error("invalid ELF segment alignment");
        low = std::min<u64>(low, segment.p_vaddr & ~u64{page_size - 1});
        high = std::max(high, Up(Add(segment.p_vaddr, segment.p_memsz), page_size));
        alignment = std::max<u64>(alignment, segment.p_align);
        loads.push_back(segment);
    }
    if (loads.empty()) Error("ELF contains no loadable memory");
    std::sort(loads.begin(), loads.end(), [](const auto& a, const auto& b) {
        return a.p_vaddr < b.p_vaddr;
    });
    for (std::size_t i = 1; i < loads.size(); ++i)
        if (Add(loads[i - 1].p_vaddr, loads[i - 1].p_memsz) > loads[i].p_vaddr)
            Error("overlapping ELF segment bytes are unsupported");
    constexpr u64 heap_reserve = u64{256} << 20;
    const auto span = high - low, total = Add(span, heap_reserve);
    const auto reservation_size = header.e_type == ET_DYN ? Add(total, alignment) : total;
    if (reservation_size > SIZE_MAX) Error("ELF image span exceeds host size");
    const auto reserved =
            memory.Map(header.e_type == ET_EXEC ? low : 0,
                       static_cast<std::size_t>(reservation_size),
                       PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | (header.e_type == ET_EXEC ? MAP_FIXED : 0));
    if (reserved == failed) SystemError("reserve guest ELF");
    const auto delta = header.e_type == ET_DYN ? ((low - reserved) & (alignment - 1)) : 0;
    const auto base = Add(reserved, delta);
    if (delta) memory.Unmap(reserved, static_cast<std::size_t>(delta));
    if (reservation_size > delta + total)
        memory.Unmap(base + total, static_cast<std::size_t>(reservation_size - delta - total));
    const auto actual = [&](u64 address) {
        if (address < low || address >= high) Error("ELF address lies outside its image");
        return Add(base, address - low);
    };
    LoadedImage image{actual(header.e_entry), 0, base - low, header.e_phnum, header.e_phentsize};
    std::vector<u64> boundaries{base, base + span};
    for (const auto& segment : loads) {
        const auto begin = actual(segment.p_vaddr) & ~u64{page_size - 1};
        const auto end = Up(Add(actual(segment.p_vaddr), segment.p_memsz), page_size);
        if (memory.Protect(begin, static_cast<std::size_t>(end - begin), PROT_READ | PROT_WRITE))
            SystemError("populate guest ELF");
        ReadAt(fd,
               reinterpret_cast<void*>(actual(segment.p_vaddr)),
               static_cast<std::size_t>(segment.p_filesz),
               segment.p_offset);
        // Anonymous pages start zero and actual segment byte extents do not overlap.
        boundaries.push_back(begin);
        boundaries.push_back(end);
        if (header.e_phoff >= segment.p_offset &&
            header.e_phoff - segment.p_offset <= segment.p_filesz &&
            table_size <= segment.p_filesz - (header.e_phoff - segment.p_offset))
            image.phdr = actual(segment.p_vaddr) + header.e_phoff - segment.p_offset;
    }
    if (!image.phdr) Error("ELF program headers must be covered by PT_LOAD");
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
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
        if (memory.Protect(boundaries[i - 1],
                           static_cast<std::size_t>(boundaries[i] - boundaries[i - 1]),
                           protection))
            SystemError("protect guest ELF");
    }
    if (memory.CodeAt(image.entry).empty()) Error("ELF entry is not executable/readable");
    memory.InitializeBrk(base + span, base + total);
    image.path = path;
    image.device = static_cast<u64>(status.st_dev);
    image.inode = static_cast<u64>(status.st_ino);
    image.file_size = file_size;
    return image;
}

u64 InitialStack(execution::GuestMemory& memory,
                 const LoadedImage& image,
                 std::span<const std::string> argv,
                 std::span<const std::string> environment) {
    const auto page_size = memory.PageSize();
    constexpr std::size_t stack_size = 8u << 20;
    const auto base =
            memory.Map(0, stack_size + page_size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (base == failed || memory.Protect(base + page_size, stack_size, PROT_READ | PROT_WRITE))
        SystemError("guest stack");
    const auto bottom = base + page_size;
    u64 top = bottom + stack_size;
    const auto push = [&](const void* data, std::size_t size) {
        if (size > top - bottom) Error("guest arguments exceed initial stack");
        top -= size;
        std::memcpy(reinterpret_cast<void*>(top), data, size);
        return top;
    };
    const char platform[] = "x86_64";
    const auto platform_at = push(platform, sizeof(platform));
    std::uint8_t random[16];
    const auto fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) SystemError("open AT_RANDOM source");
    std::size_t received{};
    while (received < sizeof(random)) {
        const auto count = read(fd, random + received, sizeof(random) - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            Error("read AT_RANDOM source");
        }
        received += static_cast<std::size_t>(count);
    }
    close(fd);
    const auto random_at = push(random, sizeof(random));
    std::vector<u64> arguments, variables;
    for (const auto& argument : argv)
        arguments.push_back(push(argument.c_str(), argument.size() + 1));
    for (const auto& variable : environment)
        variables.push_back(push(variable.c_str(), variable.size() + 1));
    std::vector<u64> words{argv.size()};
    words.insert(words.end(), arguments.begin(), arguments.end());
    words.push_back(0);
    words.insert(words.end(), variables.begin(), variables.end());
    words.push_back(0);
    const auto aux = [&](u64 key, u64 value) {
        words.push_back(key);
        words.push_back(value);
    };
    aux(AT_PHDR, image.phdr);
    aux(AT_PHENT, image.phentsize);
    aux(AT_PHNUM, image.phnum);
    aux(AT_PAGESZ, page_size);
    aux(AT_BASE, 0);
    aux(AT_FLAGS, 0);
    aux(AT_ENTRY, image.entry);
    aux(AT_UID, getuid());
    aux(AT_EUID, geteuid());
    aux(AT_GID, getgid());
    aux(AT_EGID, getegid());
    aux(AT_SECURE, 0);
    aux(AT_RANDOM, random_at);
    aux(AT_PLATFORM, platform_at);
    aux(AT_HWCAP, 0);
    aux(AT_HWCAP2, 0);
    aux(AT_CLKTCK, static_cast<u64>(std::max(1L, sysconf(_SC_CLK_TCK))));
    aux(AT_EXECFN, arguments.empty() ? 0 : arguments.front());
    aux(AT_NULL, 0);
    if (words.size() > (top - bottom) / 8) Error("guest stack table exceeds stack");
    top = (top - words.size() * 8) & ~u64{15};
    if (top < bottom) Error("guest stack alignment exceeds stack");
    std::memcpy(reinterpret_cast<void*>(top), words.data(), words.size() * 8);
    return top;
}

}  // namespace swift::runtime::platform
