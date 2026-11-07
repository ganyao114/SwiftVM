#pragma once

#include "aot/elf.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace swift::tests::aot_fixture {
// Keep mutable data off executable pages on 4/16/64 KiB Linux hosts.
inline constexpr std::uint64_t Base = 0x400000, Text = Base + 0x1000, Data = Base + 0x10000;
inline constexpr std::uint64_t DataOffset = Data - Base;
inline constexpr std::uint64_t Sum7 = Text + 0x40, FloatAdd = Text + 0x80, Mixed = Text + 0xa0,
                               Big = Text + 0xc0, Variadic = Text + 0x100, Increment = Text + 0x120,
                               Resolver = Text + 0x160, Nine = Text + 0x180, Cpu = Text + 0x1a0,
                               Read = Text + 0x1c0, Unsupported = Text + 0x200, Main = Text + 0x220;

inline std::vector<std::uint8_t> Guest() {
    namespace elf = ::swift::aot::elf;
    std::vector<std::uint8_t> bytes(DataOffset + 8);
    const auto code = [&](std::uint64_t address, std::initializer_list<std::uint8_t> input) {
        std::copy(input.begin(),
                  input.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(address - Base));
    };
    const auto displacement = [&](std::uint64_t slot, std::uint64_t after, std::uint64_t target) {
        const auto delta = static_cast<std::uint32_t>(target - after);
        std::memcpy(bytes.data() + slot - Base, &delta, sizeof(delta));
    };
    code(Text, {0xb8, 60, 0, 0, 0, 0xbf, 42, 0, 0, 0, 0x0f, 0x05});
    code(Sum7, {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x01, 0xd0, 0x48, 0x01, 0xc8,
                0x4c, 0x01, 0xc0, 0x4c, 0x01, 0xc8, 0x48, 0x03, 0x44, 0x24, 0x08, 0xc3});
    code(FloatAdd, {0xf2, 0x0f, 0x58, 0xc1, 0xc3});
    code(Mixed, {0x48, 0x8d, 0x47, 0x01, 0xf2, 0x0f, 0x58, 0xc0, 0xc3});
    code(Big, {0x48, 0x89, 0x37, 0x48, 0x8d, 0x46, 0x01, 0x48, 0x89, 0x47, 0x08, 0x48,
               0x83, 0xc0, 0x01, 0x48, 0x89, 0x47, 0x10, 0x48, 0x89, 0xf8, 0xc3});
    code(Variadic, {0x0f, 0xb6, 0xc0, 0xc3});
    code(Increment,
         {0x48, 0x8b, 0x05, 0, 0, 0, 0, 0x48, 0x01, 0xf8, 0x48, 0x89, 0x05, 0, 0, 0, 0, 0xc3});
    displacement(Increment + 3, Increment + 7, Data);
    displacement(Increment + 13, Increment + 17, Data);
    code(Resolver, {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
    displacement(Resolver + 3, Resolver + 7, Nine);
    code(Nine, {0xb8, 9, 0, 0, 0, 0xc3});
    code(Cpu, {0x31, 0xc0, 0x31, 0xc9, 0x0f, 0xa2, 0xc3});
    code(Read, {0x48, 0x8b, 0x07, 0xc3});
    code(Unsupported, {0x0f, 0x0b});
    code(Main, {0xc3});
    const std::uint64_t initial = 40;
    std::memcpy(bytes.data() + DataOffset, &initial, sizeof(initial));

    std::vector<std::uint8_t> strings{0};
    std::vector<elf::Symbol> symbols(1);
    const auto symbol = [&](const char* name,
                            std::uint64_t address,
                            std::uint8_t type,
                            std::uint64_t size,
                            std::uint16_t section = 1) {
        const auto offset = static_cast<std::uint32_t>(strings.size());
        strings.insert(strings.end(), name, name + std::strlen(name) + 1);
        symbols.push_back(
                {offset, static_cast<std::uint8_t>(0x10 | type), 0, section, address, size});
    };
    symbol("_start", Text, 2, 12);
    symbol("sum7", Sum7, 2, 24);
    symbol("sum_alias", Sum7, 2, 0);
    symbol("fp_add", FloatAdd, 2, 5);
    symbol("mixed", Mixed, 2, 9);
    symbol("big", Big, 2, 23);
    symbol("variadic", Variadic, 2, 4);
    symbol("increment", Increment, 2, 18);
    symbol("choose", Resolver, 10, 8);
    symbol("nine", Nine, 2, 6);
    symbol("cpu", Cpu, 2, 7);
    symbol("read", Read, 2, 4);
    symbol("unsupported", Unsupported, 2, 2);
    symbol("main", Main, 2, 1);
    symbol("global", Data, 1, 8, 2);
    std::vector<std::uint8_t> names{0};
    const auto section_name = [&](const char* name) {
        const auto offset = static_cast<std::uint32_t>(names.size());
        names.insert(names.end(), name, name + std::strlen(name) + 1);
        return offset;
    };
    std::array<elf::Section, 7> sections{};
    sections[1] = {section_name(".text"), elf::Progbits, 6, Text, 0x1000, 0x221, 0, 0, 16, 0};
    sections[2] = {section_name(".data"), elf::Progbits, 3, Data, DataOffset, 8, 0, 0, 8, 0};
    sections[3] = {
            section_name(".bss"), elf::Nobits, 3, Data + 8, DataOffset + 8, 0xff8, 0, 0, 8, 0};
    const auto append = [&](const void* data, std::size_t size, std::size_t align = 1) {
        bytes.resize((bytes.size() + align - 1) & ~(align - 1));
        const auto offset = bytes.size();
        const auto* input = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), input, input + size);
        return static_cast<std::uint64_t>(offset);
    };
    sections[4] = {section_name(".symtab"),
                   elf::Symtab,
                   0,
                   0,
                   append(symbols.data(), symbols.size() * sizeof(elf::Symbol), 8),
                   symbols.size() * sizeof(elf::Symbol),
                   5,
                   1,
                   8,
                   sizeof(elf::Symbol)};
    sections[5] = {section_name(".strtab"),
                   elf::Strtab,
                   0,
                   0,
                   append(strings.data(), strings.size()),
                   strings.size(),
                   0,
                   0,
                   1,
                   0};
    const auto names_name = section_name(".shstrtab");
    sections[6] = {names_name,
                   elf::Strtab,
                   0,
                   0,
                   append(names.data(), names.size()),
                   names.size(),
                   0,
                   0,
                   1,
                   0};
    elf::Header header;
    constexpr std::uint8_t identity[]{0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(header.ident, identity, sizeof(identity));
    header.type = elf::ExecutableFile;
    header.machine = elf::X86;
    header.version = 1;
    header.entry = Text;
    header.phoff = sizeof(header);
    header.ehsize = sizeof(header);
    header.phentsize = sizeof(elf::ProgramHeader);
    header.phnum = 2;
    header.shentsize = sizeof(elf::Section);
    header.shnum = sections.size();
    header.shstrndx = 6;
    header.shoff = append(sections.data(), sizeof(sections), 8);
    const elf::ProgramHeader segments[]{{elf::Load, 5, 0, Base, Base, 0x1800, 0x1800, 4096},
                                        {elf::Load, 6, DataOffset, Data, Data, 8, 0x1000, 4096}};
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + header.phoff, segments, sizeof(segments));
    return bytes;
}

inline std::vector<std::uint8_t> StartupGuest() {
    auto bytes = Guest();
    // _start: movq $0x1234,global(%rip); jmp main. main will be patched on
    // disk before compilation; the original file remains an ordinary ELF.
    constexpr std::uint8_t body[]{0x48, 0xc7, 0x05, 0, 0, 0, 0, 0x34, 0x12, 0, 0, 0xe9, 0, 0, 0, 0};
    std::memcpy(bytes.data() + 0x1000, body, sizeof(body));
    const auto store = static_cast<std::uint32_t>(Data - (Text + 11));
    const auto jump = static_cast<std::uint32_t>(Main - (Text + 16));
    std::memcpy(bytes.data() + 0x1003, &store, sizeof(store));
    std::memcpy(bytes.data() + 0x100c, &jump, sizeof(jump));
    return bytes;
}
}  // namespace swift::tests::aot_fixture
