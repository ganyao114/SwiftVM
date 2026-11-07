#include "elf.h"

#include "base/error.h"

#include <bit>
#include <cstring>
#include <limits>

namespace swift::aot::elf {
namespace {
[[noreturn]] void Invalid(const char* text) { ThrowError(text, ErrorKind::InvalidArgument); }

template <class T>
std::vector<T> Table(std::span<const std::uint8_t> bytes, std::uint64_t offset, std::size_t count) {
    if (offset > bytes.size() || count > (bytes.size() - offset) / sizeof(T))
        Invalid("ELF table exceeds file");
    std::vector<T> result(count);
    if (count) std::memcpy(result.data(), bytes.data() + offset, count * sizeof(T));
    return result;
}
}  // namespace

View::View(std::span<const std::uint8_t> input) : bytes(input) {
    static_assert(std::endian::native == std::endian::little);
    if (bytes.size() < sizeof(Header)) Invalid("truncated ELF header");
    std::memcpy(&header, bytes.data(), sizeof(header));
    constexpr std::uint8_t identity[]{0x7f, 'E', 'L', 'F', 2, 1, 1};
    if (std::memcmp(header.ident, identity, sizeof(identity)) || header.version != 1 ||
        header.ehsize != sizeof(Header))
        Invalid("expected ELF64 little-endian version 1");
    if ((header.phnum && header.phentsize != sizeof(ProgramHeader)) ||
        (header.shnum && header.shentsize != sizeof(Section)))
        Invalid("unsupported ELF table entry size");
    segments = Table<ProgramHeader>(bytes, header.phoff, header.phnum);
    sections = Table<Section>(bytes, header.shoff, header.shnum);
    if (header.shstrndx >= sections.size() && header.shstrndx)
        Invalid("ELF section-name table index exceeds table");
    for (const auto& segment : segments) {
        if (segment.offset > bytes.size() || segment.filesz > bytes.size() - segment.offset ||
            (segment.type == Load && segment.filesz > segment.memsz))
            Invalid("ELF segment exceeds file/memory bounds");
    }
    for (const auto& section : sections) {
        if (section.type != Nobits) (void)Contents(section);
        if (section.type == Symtab &&
            (section.entry_size != sizeof(Symbol) || section.size % sizeof(Symbol) ||
             section.link >= sections.size() || sections[section.link].type != Strtab))
            Invalid("invalid ELF symbol table");
        if (section.align && (section.align & (section.align - 1)))
            Invalid("invalid ELF section alignment");
    }
    if (!sections.empty() && header.shstrndx) {
        if (sections[header.shstrndx].type != Strtab) Invalid("invalid ELF section-name table");
        for (const auto& section : sections) (void)Name(section);
    }
}

std::span<const std::uint8_t> View::Contents(const Section& section) const {
    if (section.type == Nobits) return {};
    if (section.offset > bytes.size() || section.size > bytes.size() - section.offset)
        Invalid("ELF section exceeds file");
    return bytes.subspan(static_cast<std::size_t>(section.offset),
                         static_cast<std::size_t>(section.size));
}

std::string_view View::String(const Section& table, std::uint32_t offset) const {
    const auto contents = Contents(table);
    if (offset >= contents.size()) Invalid("ELF string offset exceeds table");
    const auto* begin = reinterpret_cast<const char*>(contents.data() + offset);
    const auto* end = static_cast<const char*>(std::memchr(begin, 0, contents.size() - offset));
    if (!end) Invalid("unterminated ELF string");
    return {begin, static_cast<std::size_t>(end - begin)};
}

std::string_view View::Name(const Section& section) const {
    if (!header.shstrndx) return {};
    return String(sections[header.shstrndx], section.name);
}

std::string_view View::SymbolName(const Section& table, const Symbol& symbol) const {
    if (table.link >= sections.size()) Invalid("ELF symbol string table is missing");
    return String(sections[table.link], symbol.name);
}

std::vector<Symbol> View::Symbols(const Section& table) const {
    if (table.type != Symtab || table.entry_size != sizeof(Symbol) || table.size % sizeof(Symbol))
        Invalid("invalid ELF symbol table");
    return Table<Symbol>(
            bytes, table.offset, static_cast<std::size_t>(table.size / sizeof(Symbol)));
}

const Section* View::Find(std::string_view name) const {
    const Section* found{};
    for (const auto& section : sections) {
        if (Name(section) != name) continue;
        if (found) Invalid("duplicate ELF section name");
        found = &section;
    }
    return found;
}
}  // namespace swift::aot::elf
