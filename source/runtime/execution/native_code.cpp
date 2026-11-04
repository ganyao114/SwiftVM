#include "native_code.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace swift::runtime::execution {
namespace {
[[noreturn]] void Failed(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

bool Imm19Link(std::uint32_t instruction) noexcept {
    return (instruction & UINT32_C(0x7e000000)) == UINT32_C(0x34000000) || // CBZ/CBNZ W/X.
           (instruction & UINT32_C(0xff000010)) == UINT32_C(0x54000000); // B.cond.
}

void WriteLinkInstruction(std::uintptr_t* writable, const std::uintptr_t* executable,
                          std::uint32_t back_words, std::uint32_t instruction) noexcept {
    auto* write = reinterpret_cast<std::uint32_t*>(writable) - back_words;
    auto* execute = reinterpret_cast<const std::uint32_t*>(executable) - back_words;
    if (__atomic_load_n(write, __ATOMIC_RELAXED) == instruction) return;
    __atomic_store_n(write, instruction, __ATOMIC_RELEASE);
    __builtin___clear_cache(reinterpret_cast<char*>(write), reinterpret_cast<char*>(write + 1));
    if (write != execute)
        __builtin___clear_cache(reinterpret_cast<char*>(const_cast<std::uint32_t*>(execute)),
                                reinterpret_cast<char*>(const_cast<std::uint32_t*>(execute + 1)));
}
}

void PatchNativeLink(std::uintptr_t* writable, const std::uintptr_t* executable,
                     std::uintptr_t target, std::uint32_t back_words) noexcept {
    // Keep the far fallback ready before publishing a direct instruction.
    __atomic_store_n(writable, target, __ATOMIC_RELEASE);
    const auto head = __atomic_load_n(reinterpret_cast<std::uint32_t*>(writable) - back_words, __ATOMIC_RELAXED);
    const bool conditional = Imm19Link(head);
    const auto gate_back = conditional ? 2u : back_words;
    const auto from = reinterpret_cast<std::uintptr_t>(executable) - gate_back * sizeof(std::uint32_t);
    const auto delta = static_cast<std::int64_t>(target - from);
    const auto instruction = !(delta & 3) && delta >= -(INT64_C(1) << 27) && delta < (INT64_C(1) << 27)
            ? UINT32_C(0x14000000) | (static_cast<std::uint32_t>(delta / 4) & UINT32_C(0x03ffffff))
            : UINT32_C(0x58000010) | (gate_back << 5); // Original LDR x16,DATA.
    WriteLinkInstruction(writable, executable, gate_back, instruction);
    if (conditional) {
        const auto head_from = reinterpret_cast<std::uintptr_t>(executable) - back_words * sizeof(std::uint32_t);
        const auto distance = static_cast<std::int64_t>(target - head_from);
        const auto words = !(distance & 3) && distance >= -(INT64_C(1) << 20) && distance < (INT64_C(1) << 20)
                ? static_cast<std::uint32_t>(distance / 4) : back_words - 2;
        WriteLinkInstruction(writable, executable, back_words,
                (head & ~(UINT32_C(0x7ffff) << 5)) | ((words & UINT32_C(0x7ffff)) << 5));
    }
}

void UnlinkNativeLink(std::uintptr_t* writable, const std::uintptr_t* executable,
                      std::uint32_t back_words) noexcept {
    __atomic_store_n(writable, reinterpret_cast<std::uintptr_t>(executable + 1), __ATOMIC_RELEASE);
    const auto head = __atomic_load_n(reinterpret_cast<std::uint32_t*>(writable) - back_words, __ATOMIC_RELAXED);
    const bool conditional = Imm19Link(head);
    const auto gate_back = conditional ? 2u : back_words;
    WriteLinkInstruction(writable, executable, gate_back, UINT32_C(0x58000010) | (gate_back << 5));
    if (conditional)
        WriteLinkInstruction(writable, executable, back_words,
                (head & ~(UINT32_C(0x7ffff) << 5)) | ((back_words - 2) << 5));
}

CodeArena::~CodeArena() { Clear(); }
void CodeArena::Clear() noexcept {
    for (const auto& slab : slabs) {
        munmap(slab.write, slab.size);
        if (slab.dual) munmap(slab.execute, slab.size);
    }
    std::vector<Slab>{}.swap(slabs);
    std::vector<std::vector<backend::arm64::FaultSite>>{}.swap(fault_tables);
}
std::size_t CodeArena::DualSlabs() const noexcept {
    return static_cast<std::size_t>(std::count_if(slabs.begin(), slabs.end(), [](const Slab& slab) { return slab.dual; }));
}
std::size_t CodeArena::ReservedBytes() const noexcept {
    std::size_t bytes{}; for (const auto& slab : slabs) bytes += slab.size * (slab.dual ? 2 : 1); return bytes;
}

NativeCode CodeArena::Publish(std::span<const std::uint32_t> words, std::vector<backend::arm64::FaultSite> faults,
                              std::span<const backend::arm64::LinkSite> links) {
    if (words.empty() || words.size() > (std::numeric_limits<std::size_t>::max() - 4096) / 4)
        throw std::runtime_error("invalid native code size");
    const auto bytes = words.size() * 4;
    std::span<const backend::arm64::FaultSite> sites;
    if (!faults.empty()) {
        fault_tables.push_back(std::move(faults));
        sites = fault_tables.back();
    }
    if (slabs.empty() || bytes > slabs.back().size - ((slabs.back().used + 15) & ~std::size_t{15})) {
        const auto page_result = sysconf(_SC_PAGESIZE);
        if (page_result <= 0) Failed("code page size");
        const auto page = static_cast<std::size_t>(page_result);
        if (bytes > SIZE_MAX - page - 15) throw std::runtime_error("native slab size overflow");
        const auto size = (std::max(std::size_t{1} << 20, bytes + 15) + page - 1) / page * page;
        void* write = MAP_FAILED; void* execute = MAP_FAILED;
        bool dual{};
#if defined(SYS_memfd_create)
        const auto fd = static_cast<int>(syscall(SYS_memfd_create, "swiftvm-code", 1u)); // MFD_CLOEXEC
        if (fd >= 0) {
            if (ftruncate(fd, static_cast<off_t>(size)) == 0) {
                write = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (write != MAP_FAILED) execute = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
            }
            close(fd);
            dual = write != MAP_FAILED && execute != MAP_FAILED;
            if (!dual && write != MAP_FAILED) { munmap(write, size); write = MAP_FAILED; }
        }
#endif
        if (!dual) {
            write = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (write == MAP_FAILED) Failed("code slab mmap");
            execute = write;
        }
        try { slabs.push_back({static_cast<std::uint8_t*>(write), static_cast<std::uint8_t*>(execute), size, 0, dual}); }
        catch (...) { munmap(write, size); if (dual) munmap(execute, size); throw; }
    }
    auto& slab = slabs.back();
    const auto offset = (slab.used + 15) & ~std::size_t{15};
    if (!slab.dual && mprotect(slab.write, slab.size, PROT_READ | PROT_WRITE)) Failed("code slab writable");
    auto* write = slab.write + offset; auto* execute = slab.execute + offset;
    std::memcpy(write, words.data(), bytes);
    // The actual RX address exists only now. Initialize each DATA pointer
    // while this mapping is writable, including the single-mapping fallback.
    for (const auto& link : links) {
        const auto pointer = std::size_t{link.pointer_word}, branch = std::size_t{link.branch_word};
        const auto fallback = pointer + 2;
        bool valid = !(pointer & 1) && branch < pointer && fallback < words.size();
        if (valid && Imm19Link(words[branch])) {
            const auto gate = pointer - 2;
            valid = branch < gate && gate - branch < (std::size_t{1} << 18) &&
                    ((words[branch] >> 5) & 0x7ffffu) == gate - branch &&
                    words[gate] == 0x58000050u && words[gate + 1] == 0xd61f0200u;
        } else if (valid) valid = pointer - branch == 2 + (branch & 1);
        if (!valid) {
            // This slice is unpublished. Preserve older entries in a shared
            // W^X slab when rejecting the current actual relocation.
            if (!slab.dual && mprotect(slab.execute, slab.size, PROT_READ | PROT_EXEC)) Failed("code slab executable");
            throw std::runtime_error("invalid native cold-link relocation");
        }
        const auto target = reinterpret_cast<std::uintptr_t>(execute + fallback * 4);
        std::memcpy(write + pointer * 4, &target, sizeof(target));
    }
    __builtin___clear_cache(reinterpret_cast<char*>(write), reinterpret_cast<char*>(write + bytes));
    if (!slab.dual && mprotect(slab.execute, slab.size, PROT_READ | PROT_EXEC)) Failed("code slab executable");
    if (execute != write)
        __builtin___clear_cache(reinterpret_cast<char*>(execute), reinterpret_cast<char*>(execute + bytes));
    slab.used = offset + bytes;
    return {reinterpret_cast<NativeCode::Function>(execute), bytes, slab.dual ? write : nullptr, sites};
}

} // namespace swift::runtime::execution
