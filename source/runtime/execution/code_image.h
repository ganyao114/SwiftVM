#pragma once

#include "base/hash.h"
#include "runtime/backend/code.h"
#include "runtime/code_source.h"

#include <span>
#include <string>
#include <vector>

namespace swift::runtime::execution {
struct HostBackendOps;
struct RuntimeOptions;

inline constexpr std::uint32_t CodeImageFormatVersion = 2;

struct CodeImageKey {
    Digest build_id{}, guest_id{}, configuration{};
    std::uint64_t load_bias{};
    std::uint32_t host_isa{};
    ir::Scope requested_scope{ir::Scope::Block};
    friend bool operator==(const CodeImageKey&, const CodeImageKey&) = default;
};

struct SourceRecord {
    SourceSpan span;
    Digest digest{};
    std::uint8_t mode{};  // SourceMode: Immutable or WatchedBlock at compilation.
};

struct CodeUnit {
    std::uint64_t pc{};
    std::uint8_t fp_mode{};
    ir::Scope scope{ir::Scope::Block};
    backend::CompileResult code;
    std::vector<SourceRecord> sources;
};

struct CodeImage {
    CodeImageKey key;
    std::vector<CodeUnit> units;
};

CodeImageKey MakeCodeImageKey(const Digest& guest,
                              std::uint64_t load_bias,
                              const HostBackendOps&,
                              const RuntimeOptions&);
Digest CodeImageKeyDigest(const CodeImageKey&);

// Disk caches carry words inline. AOT ELF carries the same unit records with
// code stored once in .svmaot.text, referenced by byte offsets in metadata.
std::vector<std::uint8_t> EncodeCodeImage(const CodeImage&);
bool DecodeCodeImage(std::span<const std::uint8_t>, CodeImage&, std::string& error);
std::vector<std::uint64_t> CodeUnitTextOffsets(const CodeImage&);
std::vector<std::uint8_t> EncodeCodeMetadata(const CodeImage&,
                                             std::vector<std::uint8_t>& text,
                                             std::span<const std::uint8_t> tail = {});
bool DecodeCodeMetadata(std::span<const std::uint8_t> metadata,
                        std::span<const std::uint8_t> text,
                        CodeImage&,
                        std::string& error);
}  // namespace swift::runtime::execution
