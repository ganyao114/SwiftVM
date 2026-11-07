#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace swift::runtime::platform {
inline constexpr std::size_t ArtifactFileLimit = 256u << 20;

std::vector<std::uint8_t> ReadArtifactFile(const std::string&,
                                           bool private_owner = false,
                                           std::size_t limit = ArtifactFileLimit);
void WriteArtifactFile(const std::string&, std::span<const std::uint8_t>);
// Cache directories are private and must not be shared with untrusted writers.
void EnsureCacheDirectory(const std::string&);
}  // namespace swift::runtime::platform
