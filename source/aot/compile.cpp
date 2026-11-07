#include "compile.h"

#include "base/error.h"
#include "elf.h"
#include "runtime/execution/offline_compile.h"
#include "runtime/platform/linux/artifact_file.h"

#include <algorithm>

namespace swift::aot {
bool CompileArtifact(const CompileOptions& options, ArtifactStats& stats, std::string& error) {
    try {
        Artifact artifact;
        artifact.guest_elf = runtime::platform::ReadArtifactFile(options.guest_elf);
        artifact.guest_path = options.guest_elf;
        const elf::View guest{artifact.guest_elf};
        auto entries = options.entries;
        entries.push_back(guest.FileHeader().entry);
        // DiscoverSymbols also validates the supported static x86-64 image.
        for (const auto& symbol : DiscoverSymbols(artifact.guest_elf))
            if (options.symbols && symbol.Function()) entries.push_back(symbol.guest_value);
        std::sort(entries.begin(), entries.end());
        entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
        if (options.max_entries && entries.size() > options.max_entries)
            entries.resize(options.max_entries);
        artifact.stats.candidates = entries.size();
        auto policy = options.runtime;
        policy.mode = runtime::execution::ExecutionMode::JitOnly;
        policy.cache_directory.clear();
        policy.artifact_path.clear();
        runtime::execution::RuntimeStats compiled;
        artifact.code = runtime::execution::CompileGuestImage(
                artifact.guest_elf, artifact.guest_path, entries, options.sweep, policy, compiled);
        artifact.stats.rejected = compiled.compile_attempts > artifact.code.units.size()
                                          ? compiled.compile_attempts - artifact.code.units.size()
                                          : 0;
        ResolveSymbols(artifact);
        if (artifact.code.units.empty())
            ThrowError("AOT did not produce any native units", ErrorKind::Unsupported);
        if (!WriteArtifact(options.output, artifact, error)) return false;
        stats = artifact.stats;
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}
}  // namespace swift::aot
