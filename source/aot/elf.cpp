#include "elf.h"

#include "base/error.h"

#include <bit>
#include <cstring>
#include <limits>

namespace swift::aot::elf {
namespace {
struct Header32 {
    std::uint8_t ident[16];
    std::uint16_t type, machine;
    std::uint32_t version, entry, phoff, shoff, flags;
    std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Program32 {
    std::uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

struct Section32 {
    std::uint32_t name, type, flags, address, offset, size, link, info, align, entry_size;
};

struct Symbol32 {
    std::uint32_t name, value, size;
    std::uint8_t info, other;
    std::uint16_t section;
};

static_assert(sizeof(Header32) == 52 && sizeof(Program32) == 32 && sizeof(Section32) == 40 &&
              sizeof(Symbol32) == 16);

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
    if (bytes.size() < 16) Invalid("truncated ELF header");
    constexpr std::uint8_t magic[]{0x7f, 'E', 'L', 'F'};
    if (std::memcmp(bytes.data(), magic, sizeof(magic)) || bytes[5] != 1 || bytes[6] != 1 ||
        (bytes[4] != 1 && bytes[4] != 2))
        Invalid("expected ELF32/ELF64 little-endian version 1");
    if (bytes[4] == 1) {
        if (bytes.size() < sizeof(Header32)) Invalid("truncated ELF32 header");
        Header32 raw;
        std::memcpy(&raw, bytes.data(), sizeof(raw));
        std::memcpy(header.ident, raw.ident, sizeof(raw.ident));
        header.type = raw.type;
        header.machine = raw.machine;
        header.version = raw.version;
        header.entry = raw.entry;
        header.phoff = raw.phoff;
        header.shoff = raw.shoff;
        header.flags = raw.flags;
        header.ehsize = raw.ehsize;
        header.phentsize = raw.phentsize;
        header.phnum = raw.phnum;
        header.shentsize = raw.shentsize;
        header.shnum = raw.shnum;
        header.shstrndx = raw.shstrndx;
        if (raw.ehsize != sizeof(Header32) || (raw.phnum && raw.phentsize != sizeof(Program32)) ||
            (raw.shnum && raw.shentsize != sizeof(Section32)))
            Invalid("invalid ELF32 table sizes");
        for (const auto& p : Table<Program32>(bytes, raw.phoff, raw.phnum))
            segments.push_back(
                    {p.type, p.flags, p.offset, p.vaddr, p.paddr, p.filesz, p.memsz, p.align});
        for (const auto& p : Table<Section32>(bytes, raw.shoff, raw.shnum))
            sections.push_back({p.name,
                                p.type,
                                p.flags,
                                p.address,
                                p.offset,
                                p.size,
                                p.link,
                                p.info,
                                p.align,
                                p.entry_size});
    } else {
        if (bytes.size() < sizeof(Header)) Invalid("truncated ELF64 header");
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.ehsize != sizeof(Header) ||
            (header.phnum && header.phentsize != sizeof(ProgramHeader)) ||
            (header.shnum && header.shentsize != sizeof(Section)))
            Invalid("invalid ELF64 table sizes");
        segments = Table<ProgramHeader>(bytes, header.phoff, header.phnum);
        sections = Table<Section>(bytes, header.shoff, header.shnum);
    }
    if (header.version != 1) Invalid("invalid ELF version");
    if (header.shstrndx >= sections.size() && header.shstrndx)
        Invalid("ELF section-name table index exceeds table");
    for (const auto& segment : segments) {
        if (segment.offset > bytes.size() || segment.filesz > bytes.size() - segment.offset ||
            (segment.type == Load && segment.filesz > segment.memsz))
            Invalid("ELF segment exceeds file/memory bounds");
    }
    const auto symbol_size = header.ident[4] == 1 ? sizeof(Symbol32) : sizeof(Symbol);
    for (const auto& section : sections) {
        if (section.type != Nobits) (void)Contents(section);
        if (section.type == Symtab &&
            (section.entry_size != symbol_size || section.size % symbol_size ||
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
    const auto width = header.ident[4] == 1 ? sizeof(Symbol32) : sizeof(Symbol);
    if (table.type != Symtab || table.entry_size != width || table.size % width)
        Invalid("invalid ELF symbol table");
    if (header.ident[4] == 2) return Table<Symbol>(bytes, table.offset, table.size / width);
    std::vector<Symbol> result;
    for (const auto& s : Table<Symbol32>(bytes, table.offset, table.size / width))
        result.push_back({s.name, s.info, s.other, s.section, s.value, s.size});
    return result;
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
