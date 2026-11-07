#pragma once

#include "artifact.h"
#include "runtime/execution/runtime.h"

namespace swift::aot {
struct CompileOptions {
    std::string guest_elf, output;
    runtime::execution::RuntimeOptions runtime;
    std::vector<std::uint64_t> entries;
    bool symbols{true}, sweep{true};
    std::uint32_t max_entries{};
};

// Offline candidate discovery differs from demand-driven JIT. Actual decode,
// optimization, allocation, emission and publication use CompileGuestImage.
bool CompileArtifact(const CompileOptions&, ArtifactStats&, std::string& error);
}  // namespace swift::aot
