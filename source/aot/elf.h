#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swift::aot::elf {
inline constexpr std::uint32_t Progbits = 1, Symtab = 2, Strtab = 3, Nobits = 8;
inline constexpr std::uint64_t Alloc = 2, Executable = 4;
inline constexpr std::uint32_t Load = 1;
inline constexpr std::uint16_t ExecutableFile = 2, SharedFile = 3;
inline constexpr std::uint16_t Arm32 = 40, X86 = 62, Arm64 = 183, Riscv64 = 243, LoongArch64 = 258;

struct Header {
    std::uint8_t ident[16]{};
    std::uint16_t type{}, machine{};
    std::uint32_t version{};
    std::uint64_t entry{}, phoff{}, shoff{};
    std::uint32_t flags{};
    std::uint16_t ehsize{}, phentsize{}, phnum{}, shentsize{}, shnum{}, shstrndx{};
};

struct ProgramHeader {
    std::uint32_t type{}, flags{};
    std::uint64_t offset{}, vaddr{}, paddr{}, filesz{}, memsz{}, align{};
};

struct Section {
    std::uint32_t name{}, type{};
    std::uint64_t flags{}, address{}, offset{}, size{};
    std::uint32_t link{}, info{};
    std::uint64_t align{}, entry_size{};
};

struct Symbol {
    std::uint32_t name{};
    std::uint8_t info{}, other{};
    std::uint16_t section{};
    std::uint64_t value{}, size{};
};

static_assert(sizeof(Header) == 64 && sizeof(ProgramHeader) == 56);
static_assert(sizeof(Section) == 64 && sizeof(Symbol) == 24);

// A bounded ELF32/ELF64 little-endian view, normalized to 64-bit records. It neither maps PT_LOAD
// nor resolves relocations, so the AOT reader stays portable and independent of Linux VM.
class View {
public:
    explicit View(std::span<const std::uint8_t>);

    const Header& FileHeader() const noexcept { return header; }

    std::span<const ProgramHeader> Segments() const noexcept { return segments; }

    std::span<const Section> Sections() const noexcept { return sections; }

    std::span<const std::uint8_t> Contents(const Section&) const;
    std::string_view Name(const Section&) const;
    std::string_view SymbolName(const Section& table, const Symbol&) const;
    std::vector<Symbol> Symbols(const Section&) const;
    const Section* Find(std::string_view) const;

private:
    std::span<const std::uint8_t> bytes;
    Header header;
    std::vector<ProgramHeader> segments;
    std::vector<Section> sections;
    std::string_view String(const Section&, std::uint32_t) const;
};
}  // namespace swift::aot::elf
