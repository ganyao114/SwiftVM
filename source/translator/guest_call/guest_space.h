//
// A small, self-contained guest address space + static-ELF image loader, used
// to host guest code that the call layer calls into.
//
// This deliberately does NOT reuse source/translator/linux/{guest_memory,
// loader}.cpp: those are compiled into the svm_translator_linux executable
// rather than a library, and the call layer must not depend on the launcher.
// What is here is the subset the call layer needs -- a bounded guest window
// with a host bias, a bump arena, PT_LOAD placement and .symtab lookup -- and
// nothing else.  Address model matches the launcher exactly:
//
//     host address = (guest address & mask) + bias
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace swift::guest_call {

class GuestSpace {
public:
    static constexpr std::uint64_t kHostPageSize = 0x4000;  // macOS arm64
    static constexpr std::uint32_t kHostPageShift = 14;

    GuestSpace() = default;
    ~GuestSpace();
    GuestSpace(const GuestSpace&) = delete;
    GuestSpace& operator=(const GuestSpace&) = delete;

    // Reserves [0, 2^bits) of guest address space as one PROT_NONE host
    // mapping and installs the bias.  Every later mapping is MAP_FIXED inside
    // it, so no guest address can name host memory outside the reservation.
    bool ReserveWindow(std::uint32_t bits = 32);

    [[nodiscard]] std::uint64_t Bias() const { return bias_; }
    [[nodiscard]] std::uint64_t Mask() const { return mask_; }
    [[nodiscard]] std::uint32_t WindowBits() const { return bits_; }

    [[nodiscard]] void* ToHost(std::uint64_t guest) const {
        return reinterpret_cast<void*>((guest & mask_) + bias_);
    }

    bool MapFixed(std::uint64_t addr, std::uint64_t size);
    [[nodiscard]] bool RangeIsMapped(std::uint64_t addr, std::uint64_t size) const;
    [[nodiscard]] std::uint64_t MappedBytesFrom(std::uint64_t addr, std::uint64_t length) const;

    // Bump arena for scratch (MEMORY return buffers, ScopedGuestBuffer).
    // Freeing is a no-op except for the most recent allocation; the arena is
    // sized so that a test run cannot exhaust it.
    bool CreateArena(std::uint64_t base, std::uint64_t size);
    std::uint64_t ArenaAlloc(std::uint64_t size, std::uint64_t align);
    void ArenaFree(std::uint64_t addr, std::uint64_t size);

    // --- static ELF64 loading (x86-64, ET_EXEC or ET_DYN at its link addr) ---
    struct Image {
        std::uint64_t entry{};
        std::uint64_t min_vaddr{};
        std::uint64_t max_vaddr{};
        std::uint64_t phdr{};      // guest address of the in-memory phdr table
        std::uint64_t phentsize{};
        std::uint64_t phnum{};
        std::unordered_map<std::string, std::uint64_t> functions;  // STT_FUNC / IFUNC
        std::unordered_map<std::string, std::uint64_t> objects;    // STT_OBJECT
        // STT_GNU_IFUNC symbols: st_value is the RESOLVER, not the function.
        // Calling one directly returns an address instead of doing the work --
        // exactly the kind of silent wrong answer this project cares about, so
        // they are tracked separately and resolved before use.
        std::unordered_set<std::string> ifuncs;
        std::vector<std::uint64_t> irelative_targets;  // R_X86_64_IRELATIVE slots
        std::vector<std::uint64_t> irelative_resolvers;
    };

    // Loads the ELF at `path` into this space.  Returns false and leaves
    // `error` set on any problem.
    bool LoadElf(const std::string& path, Image& out, std::string& error);

private:
    void SetPageBits(std::uint64_t addr, std::uint64_t size, bool present);
    [[nodiscard]] bool PageBit(std::uint64_t addr) const;

    std::uint64_t bias_{};
    std::uint64_t mask_{~std::uint64_t{0}};
    std::uint32_t bits_{};
    std::uint64_t window_size_{};
    std::unique_ptr<std::uint64_t[]> page_bitmap_;
    std::uint64_t arena_base_{};
    std::uint64_t arena_end_{};
    std::uint64_t arena_next_{};
};

}  // namespace swift::guest_call
