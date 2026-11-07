#pragma once

#include "runtime/execution/code_image.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace swift::aot {
inline constexpr std::uint32_t ArtifactVersion = 1;
inline constexpr std::uint64_t CodeVirtualAddress = UINT64_C(0x800000000000);

struct Symbol {
    std::string name;
    std::uint64_t guest_value{}, size{};
    std::uint16_t section{};
    std::uint8_t type{}, binding{}, visibility{};
    // Only independently callable entries are exported. IFUNC names its
    // resolver, retaining the original ELF meaning.
    std::uint32_t unit{UINT32_MAX}, code_offset{};

    bool Function() const noexcept { return type == 2 || type == 10; }

    bool Compiled() const noexcept { return unit != UINT32_MAX; }
};

struct ArtifactStats {
    std::uint64_t candidates{}, compiled_units{}, rejected{}, functions{}, stubbed{};
};

struct Artifact {
    runtime::execution::CodeImage code;
    std::vector<std::uint8_t> guest_elf;
    std::string guest_path;
    std::vector<Symbol> symbols;
    ArtifactStats stats;
};

// These codecs perform no guest mapping or execution. The host ELF carries
// one copy of native text and one complete original guest ELF.
std::vector<std::uint8_t> EncodeArtifact(const Artifact&);
bool DecodeArtifact(std::span<const std::uint8_t>, Artifact&, std::string& error);
bool ReadArtifact(const std::string& path, Artifact&, std::string& error);
bool WriteArtifact(const std::string& path, const Artifact&, std::string& error);

// Symbol discovery reads the embedded ELF and resolves only public native
// entries; it does not infer an ABI for shared-SSA interior blocks.
std::vector<Symbol> DiscoverSymbols(std::span<const std::uint8_t> guest_elf);
void ResolveSymbols(Artifact&);
const Symbol* FindSymbol(const Artifact&, const std::string&) noexcept;

// Patch the input snapshot before compilation. The running AOT image is
// never edited, because its source identity must remain the compiled one.
bool PatchGuestStopByte(const std::string& guest,
                        const std::string& symbol,
                        const std::string& output,
                        std::string& error);
}  // namespace swift::aot
