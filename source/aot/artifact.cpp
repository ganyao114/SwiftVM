#include "artifact.h"

#include "base/error.h"
#include "elf.h"
#include "runtime/platform/linux/artifact_file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace swift::aot {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> Magic{'S', 'V', 'M', 'A', 'O', 'T', '2', 0};
constexpr std::uint64_t FileLimit = runtime::platform::ArtifactFileLimit;

[[noreturn]] void Invalid(const char* text) { ThrowError(text, ErrorKind::InvalidArgument); }

struct Writer {
    Bytes bytes;

    void Number(std::uint64_t value, unsigned count = 8) {
        for (unsigned i = 0; i < count; ++i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }

    void Append(std::span<const std::uint8_t> input) {
        bytes.insert(bytes.end(), input.begin(), input.end());
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;

    std::span<const std::uint8_t> Take(std::size_t size) {
        if (size > bytes.size()) Invalid("truncated AOT metadata");
        const auto out = bytes.first(size);
        bytes = bytes.subspan(size);
        return out;
    }

    std::uint64_t Number(unsigned count = 8) {
        const auto input = Take(count);
        std::uint64_t out{};
        for (unsigned i = 0; i < count; ++i) out |= std::uint64_t{input[i]} << (i * 8);
        return out;
    }
};

std::uint16_t Machine(std::uint32_t isa) {
    if (isa == 0xa64) return elf::Arm64;
    if (isa == 0x64) return elf::Riscv64;
    Invalid("unsupported AOT host ISA");
}

void ValidateGuest(const elf::View& guest) {
    if (guest.FileHeader().machine != elf::X86 || guest.FileHeader().type != elf::ExecutableFile)
        Invalid("AOT supports static x86-64 ET_EXEC guests only");
    for (const auto& section : guest.Sections())
        if (guest.Name(section).starts_with(".svmaot."))
            Invalid("guest ELF uses reserved AOT section names");
}

std::size_t Align(std::size_t value, std::size_t alignment) {
    if (value > FileLimit || alignment > FileLimit || value > FileLimit - alignment + 1)
        Invalid("AOT artifact exceeds file limit");
    return (value + alignment - 1) & ~(alignment - 1);
}

std::uint64_t Append(Bytes& bytes, std::span<const std::uint8_t> contents, std::size_t align) {
    const auto offset = Align(bytes.size(), align);
    if (contents.size() > FileLimit - offset) Invalid("AOT artifact exceeds file limit");
    bytes.resize(offset);
    bytes.insert(bytes.end(), contents.begin(), contents.end());
    return offset;
}

std::vector<std::uint8_t> SymbolBytes(const elf::View& guest,
                                      const elf::Section& section,
                                      const runtime::execution::CodeImage& code,
                                      std::span<const Symbol> resolved,
                                      std::uint16_t text_section,
                                      std::uint64_t stub,
                                      std::span<const std::uint64_t> offsets) {
    auto symbols = guest.Symbols(section);
    for (auto& raw : symbols) {
        const auto type = raw.info & 15;
        if ((type != 2 && type != 10) || !raw.section || raw.section >= 0xff00) continue;
        const auto symbol = std::find_if(resolved.begin(), resolved.end(), [&](const auto& item) {
            return item.guest_value == raw.value && item.Function();
        });
        raw.section = text_section;
        raw.value = CodeVirtualAddress + stub;
        raw.size = 8;
        if (symbol != resolved.end() && symbol->Compiled()) {
            const auto& unit = code.units[symbol->unit];
            raw.value = CodeVirtualAddress + offsets[symbol->unit] + symbol->code_offset;
            raw.size = unit.code.words.size() * 4 - symbol->code_offset;
        }
    }
    Bytes result(symbols.size() * sizeof(elf::Symbol));
    if (!result.empty()) std::memcpy(result.data(), symbols.data(), result.size());
    return result;
}

void VerifyOriginalSections(const elf::View& container,
                            const elf::View& guest,
                            const Artifact& artifact,
                            const elf::Section& text,
                            std::span<const std::uint64_t> offsets) {
    if (container.Sections().size() < guest.Sections().size())
        Invalid("AOT omitted original guest sections");
    const auto text_index = static_cast<std::uint16_t>(&text - container.Sections().data());
    const auto stub = text.size - 8;
    for (std::size_t i = 0; i < guest.Sections().size(); ++i) {
        const auto& original = guest.Sections()[i];
        const auto& copied = container.Sections()[i];
        if (container.Name(copied) != guest.Name(original) || copied.type != original.type ||
            copied.flags != original.flags || copied.address != original.address ||
            copied.link != original.link || copied.info != original.info ||
            copied.align != original.align || copied.entry_size != original.entry_size)
            Invalid("AOT changed an original guest section descriptor");
        const auto source = guest.Contents(original), destination = container.Contents(copied);
        if (original.type == elf::Symtab) {
            const auto expected = SymbolBytes(
                    guest, original, artifact.code, artifact.symbols, text_index, stub, offsets);
            if (!std::equal(
                        expected.begin(), expected.end(), destination.begin(), destination.end()))
                Invalid("AOT function symbol does not name its compiled entry or failure stub");
        } else if (i == guest.FileHeader().shstrndx) {
            if (destination.size() < source.size() ||
                !std::equal(source.begin(), source.end(), destination.begin()))
                Invalid("AOT changed original ELF section names");
        } else if (original.type != elf::Nobits &&
                   !std::equal(
                           source.begin(), source.end(), destination.begin(), destination.end()))
            Invalid("AOT changed an original guest section");
        if (original.type == elf::Nobits && copied.size != original.size)
            Invalid("AOT changed guest BSS extent");
    }
}
}  // namespace

std::vector<Symbol> DiscoverSymbols(std::span<const std::uint8_t> guest_bytes) {
    const elf::View guest{guest_bytes};
    ValidateGuest(guest);
    std::vector<Symbol> result;
    for (const auto& section : guest.Sections()) {
        if (section.type != elf::Symtab) continue;
        for (const auto& raw : guest.Symbols(section)) {
            if (!raw.section || raw.section >= 0xff00) continue;
            const auto type = static_cast<std::uint8_t>(raw.info & 15);
            if (type != 1 && type != 2 && type != 10) continue;
            if (raw.section >= guest.Sections().size())
                Invalid("guest symbol section exceeds table");
            result.push_back({std::string{guest.SymbolName(section, raw)},
                              raw.value,
                              raw.size,
                              raw.section,
                              type,
                              static_cast<std::uint8_t>(raw.info >> 4),
                              raw.other});
        }
    }
    return result;
}

namespace {
std::vector<Symbol> ResolvedSymbols(std::span<const std::uint8_t> guest,
                                    const runtime::execution::CodeImage& code) {
    auto symbols = DiscoverSymbols(guest);
    for (auto& symbol : symbols) {
        if (!symbol.Function()) continue;
        for (std::uint32_t i = 0; i < code.units.size(); ++i) {
            const auto& unit = code.units[i];
            if (unit.fp_mode) continue;
            if (unit.pc == symbol.guest_value) {
                symbol.unit = i;
                break;
            }
            for (const auto& entry : unit.code.entries) {
                if (entry.pc != symbol.guest_value) continue;
                symbol.unit = i;
                symbol.code_offset = entry.offset;
                break;
            }
            if (symbol.Compiled()) break;
        }
    }
    return symbols;
}

void CountSymbols(std::span<const Symbol> symbols, ArtifactStats& stats) {
    stats.functions = stats.stubbed = 0;
    for (const auto& symbol : symbols) {
        if (!symbol.Function()) continue;
        ++stats.functions;
        if (!symbol.Compiled()) ++stats.stubbed;
    }
}
}  // namespace

void ResolveSymbols(Artifact& artifact) {
    artifact.symbols = ResolvedSymbols(artifact.guest_elf, artifact.code);
    CountSymbols(artifact.symbols, artifact.stats);
    artifact.stats.compiled_units = artifact.code.units.size();
}

const Symbol* FindSymbol(const Artifact& artifact, const std::string& name) noexcept {
    const Symbol* local{};
    for (const auto& symbol : artifact.symbols) {
        if (symbol.name != name) continue;
        if (symbol.binding) return &symbol;
        local = &symbol;
    }
    return local;
}

Bytes EncodeArtifact(const Artifact& input) {
    const elf::View guest{input.guest_elf};
    ValidateGuest(guest);
    if (HashBytes(input.guest_elf) != input.code.key.guest_id || input.code.key.load_bias)
        Invalid("AOT guest content/key mismatch");
    if (input.guest_path.size() > UINT32_MAX) Invalid("AOT guest path is too long");
    const auto machine = Machine(input.code.key.host_isa);
    const std::uint32_t stub_words[]{machine == elf::Arm64 ? 0xd42140e0u : 0x00100073u, 0};
    const auto stub_bytes =
            std::span{reinterpret_cast<const std::uint8_t*>(stub_words), sizeof(stub_words)};
    Bytes text;
    const auto metadata = runtime::execution::EncodeCodeMetadata(input.code, text, stub_bytes);
    const auto offsets = runtime::execution::CodeUnitTextOffsets(input.code);
    const auto stub = text.size() - sizeof(stub_words);

    const auto symbols = ResolvedSymbols(input.guest_elf, input.code);
    auto stats = input.stats;
    CountSymbols(symbols, stats);
    Writer payload;
    for (const auto value : {input.stats.candidates,
                             std::uint64_t{input.code.units.size()},
                             input.stats.rejected,
                             stats.functions,
                             stats.stubbed})
        payload.Number(value);
    payload.Append(HashBytes(input.guest_elf));
    payload.Number(input.guest_path.size(), 4);
    payload.Number(metadata.size());
    payload.Append({reinterpret_cast<const std::uint8_t*>(input.guest_path.data()),
                    input.guest_path.size()});
    payload.Append(metadata);
    Writer manifest;
    manifest.Append(Magic);
    manifest.Number(ArtifactVersion, 4);
    manifest.Number(payload.bytes.size());
    manifest.Append(HashBytes(payload.bytes));
    manifest.Append(payload.bytes);

    auto sections = std::vector<elf::Section>{guest.Sections().begin(), guest.Sections().end()};
    if (sections.empty()) sections.emplace_back();
    const auto originals = sections.size();
    auto names_index = guest.FileHeader().shstrndx;
    Bytes names{0};
    if (names_index) {
        const auto old = guest.Contents(sections[names_index]);
        names.assign(old.begin(), old.end());
    } else {
        names_index = static_cast<std::uint16_t>(sections.size());
        sections.push_back({});
        sections.back().type = elf::Strtab;
        sections.back().align = 1;
        sections.back().name = 1;
        constexpr char shstr[] = ".shstrtab";
        names.insert(names.end(), shstr, shstr + sizeof(shstr));
    }
    const auto add_section = [&](const char* name,
                                 std::uint64_t flags,
                                 std::uint64_t address,
                                 std::uint64_t alignment) {
        if (sections.size() >= 0xff00) Invalid("too many AOT sections");
        elf::Section item;
        item.name = static_cast<std::uint32_t>(names.size());
        names.insert(names.end(), name, name + std::strlen(name) + 1);
        item.type = elf::Progbits;
        item.flags = flags;
        item.address = address;
        item.align = alignment;
        sections.push_back(item);
        return static_cast<std::uint16_t>(sections.size() - 1);
    };
    const auto guest_index = add_section(".svmaot.guest", 0, 0, 4096);
    const auto text_index =
            add_section(".svmaot.text", elf::Alloc | elf::Executable, CodeVirtualAddress, 16);
    const auto info_index = add_section(".svmaot.info", 0, 0, 8);
    std::vector<elf::ProgramHeader> segments;
    for (const auto& segment : guest.Segments())
        if (segment.type == elf::Load) segments.push_back(segment);
    segments.push_back({elf::Load,
                        5,
                        0,
                        CodeVirtualAddress,
                        CodeVirtualAddress,
                        text.size(),
                        text.size(),
                        4096});
    if (segments.size() > UINT16_MAX) Invalid("too many AOT load segments");
    Bytes result(sizeof(elf::Header) + segments.size() * sizeof(elf::ProgramHeader));
    const auto guest_offset = Append(result, input.guest_elf, 4096);
    sections[guest_index].offset = guest_offset;
    sections[guest_index].size = input.guest_elf.size();
    for (std::size_t i = 0; i < originals; ++i) {
        auto& section = sections[i];
        if (section.type == elf::Symtab) {
            const auto rewritten = SymbolBytes(
                    guest, guest.Sections()[i], input.code, symbols, text_index, stub, offsets);
            section.offset = Append(result, rewritten, 8);
        } else if (i)
            section.offset = guest_offset + section.offset;
    }
    sections[text_index].offset = Append(result, text, 4096);
    sections[text_index].size = text.size();
    sections[info_index].offset = Append(result, manifest.bytes, 8);
    sections[info_index].size = manifest.bytes.size();
    sections[names_index].offset = Append(result, names, 1);
    sections[names_index].size = names.size();
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
        segments[i].offset += guest_offset;
        segments[i].align = std::min<std::uint64_t>(segments[i].align, 4096);
    }
    segments.back().offset = sections[text_index].offset;
    elf::Header header;
    constexpr std::uint8_t identity[]{0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(header.ident, identity, sizeof(identity));
    header.type = elf::SharedFile;
    header.machine = machine;
    header.version = 1;
    header.flags = machine == elf::Riscv64 ? 4 : 0;
    header.phoff = sizeof(header);
    header.phnum = static_cast<std::uint16_t>(segments.size());
    header.ehsize = sizeof(header);
    header.phentsize = sizeof(elf::ProgramHeader);
    header.shentsize = sizeof(elf::Section);
    header.shnum = static_cast<std::uint16_t>(sections.size());
    header.shstrndx = names_index;
    header.shoff = Append(result,
                          {reinterpret_cast<const std::uint8_t*>(sections.data()),
                           sections.size() * sizeof(elf::Section)},
                          8);
    std::memcpy(result.data(), &header, sizeof(header));
    std::memcpy(result.data() + header.phoff,
                segments.data(),
                segments.size() * sizeof(elf::ProgramHeader));
    return result;
}

bool DecodeArtifact(std::span<const std::uint8_t> bytes, Artifact& output, std::string& error) {
    try {
        if (bytes.size() > FileLimit) Invalid("AOT artifact exceeds file limit");
        const elf::View container{bytes};
        const auto* guest_section = container.Find(".svmaot.guest");
        const auto* text_section = container.Find(".svmaot.text");
        const auto* info_section = container.Find(".svmaot.info");
        if (!guest_section || !text_section || !info_section) Invalid("missing AOT sections");
        Reader info{container.Contents(*info_section)};
        if (!std::equal(Magic.begin(), Magic.end(), info.Take(Magic.size()).begin()) ||
            info.Number(4) != ArtifactVersion)
            Invalid("unsupported AOT format version");
        const auto payload_size = info.Number();
        const auto digest = info.Take(32);
        if (payload_size != info.bytes.size()) Invalid("invalid AOT metadata length");
        const auto actual_digest = HashBytes(info.bytes);
        if (!std::equal(digest.begin(), digest.end(), actual_digest.begin()))
            Invalid("AOT metadata checksum mismatch");
        Artifact artifact;
        artifact.stats.candidates = info.Number();
        artifact.stats.compiled_units = info.Number();
        artifact.stats.rejected = info.Number();
        artifact.stats.functions = info.Number();
        artifact.stats.stubbed = info.Number();
        const auto guest_digest = info.Take(32);
        const auto guest = container.Contents(*guest_section);
        const auto actual_guest = HashBytes(guest);
        if (!std::equal(guest_digest.begin(), guest_digest.end(), actual_guest.begin()))
            Invalid("AOT embedded guest checksum mismatch");
        const auto path_size = info.Number(4), metadata_size = info.Number();
        const auto path = info.Take(static_cast<std::size_t>(path_size));
        artifact.guest_path.assign(reinterpret_cast<const char*>(path.data()), path.size());
        if (artifact.guest_path.find('\0') != std::string::npos) Invalid("invalid AOT guest path");
        const auto metadata = info.Take(static_cast<std::size_t>(metadata_size));
        if (!info.bytes.empty()) Invalid("trailing AOT metadata");
        const auto text = container.Contents(*text_section);
        if (text.size() < 8 ||
            !runtime::execution::DecodeCodeMetadata(metadata, text, artifact.code, error))
            Invalid(error.empty() ? "invalid AOT native metadata" : error.c_str());
        if (artifact.code.key.guest_id != actual_guest || artifact.code.key.load_bias)
            Invalid("AOT guest/key mismatch");
        const auto machine = Machine(artifact.code.key.host_isa);
        if (container.FileHeader().type != elf::SharedFile ||
            container.FileHeader().machine != machine ||
            (machine == elf::Riscv64 && container.FileHeader().flags != 4) ||
            text_section->address != CodeVirtualAddress || text_section->type != elf::Progbits ||
            text_section->flags != (elf::Alloc | elf::Executable))
            Invalid("AOT host ELF contract mismatch");
        const std::uint32_t stub[]{machine == elf::Arm64 ? 0xd42140e0u : 0x00100073u, 0};
        if (std::memcmp(text.data() + text.size() - sizeof(stub), stub, sizeof(stub)))
            Invalid("invalid AOT failure stub");
        artifact.guest_elf.assign(guest.begin(), guest.end());
        const auto declared = artifact.stats;
        ResolveSymbols(artifact);
        if (declared.compiled_units != artifact.stats.compiled_units ||
            declared.functions != artifact.stats.functions ||
            declared.stubbed != artifact.stats.stubbed)
            Invalid("AOT symbol census mismatch");
        VerifyOriginalSections(container,
                               elf::View{artifact.guest_elf},
                               artifact,
                               *text_section,
                               runtime::execution::CodeUnitTextOffsets(artifact.code));
        output = std::move(artifact);
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        output = {};
        return false;
    }
}

bool ReadArtifact(const std::string& path, Artifact& output, std::string& error) {
    try {
        return DecodeArtifact(runtime::platform::ReadArtifactFile(path), output, error);
    } catch (const std::exception& failure) {
        error = failure.what();
        output = {};
        return false;
    }
}

bool WriteArtifact(const std::string& path, const Artifact& artifact, std::string& error) {
    try {
        runtime::platform::WriteArtifactFile(path, EncodeArtifact(artifact));
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}
}  // namespace swift::aot
