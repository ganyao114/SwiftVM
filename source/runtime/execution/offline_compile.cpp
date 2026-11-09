#include "offline_compile.h"
#include "runtime/platform/linux/artifact_file.h"
#include "runtime/platform/linux/host_runtime.h"
#include "timing.h"
#include "vm.h"

#include "base/error.h"

#include <set>

namespace swift::runtime::execution {

CodeImage VM::Precompile(const std::string& path,
                         std::span<const std::uint64_t> entries,
                         bool sweep) {
    const auto bytes = platform::ReadArtifactFile(path);
    return Precompile(bytes, path, entries, sweep);
}

CodeImage VM::Precompile(std::span<const std::uint8_t> bytes,
                         const std::string& display_path,
                         std::span<const std::uint64_t> entries,
                         bool sweep) {
    CodeImage output;
    offline_output = &output;
    try {
        PrepareGuest(bytes, display_path);
        output.key = image_key;
        {
            FaultScope registration{thread.faults, uniform_data, PC()};
            const auto initial_mxcsr = NumericControl();
            const unsigned domains = backend.specializes_fp_flush ? 4 : 1;
            for (unsigned domain = 0; domain < domains; ++domain) {
                NumericControl() = (initial_mxcsr & ~UINT32_C(0x8040)) | ((domain & 1u) << 6) |
                                   ((domain >> 1) << 15);
                UpdateFPEnvironment();
                std::set<std::uint64_t> pending, attempted;
                const auto entry_key = [&](std::uint64_t value) {
                    return ISA() == GuestISA::Arm32 && !(value >> 32)
                                   ? arm32::Location::Make(
                                             value & ~1u, value & 1u, 0, Arm32State().fpscr)
                                             .Key()
                                   : value;
                };
                for (const auto entry : entries) pending.insert(entry_key(entry));
                pending.insert(entry_key(image.entry));
                while (!pending.empty()) {
                    const auto pc = *pending.begin();
                    pending.erase(pending.begin());
                    if (!attempted.insert(pc).second || Cache().Find(pc) ||
                        memory.CodeAt(CodeAddress(ISA(), pc)).empty())
                        continue;
                    PC() = pc;
                    RuntimeTimer total{stats.compile_request_ns};
                    auto code = compiler.Compile(options.scope);
                    if (!code.entry && options.scope == ir::Scope::Function) {
                        ++stats.fallbacks;
                        code = compiler.Compile(ir::Scope::Block);
                    }
                    if (!code.entry) continue;
                    const auto& compiled = *compiler.LastOutput();
                    Cache().InsertReady(pc, code, compiled.entries);
                    if (sweep) {
                        for (const auto& link : compiled.links)
                            if (link.kind == backend::LinkKind::Static)
                                pending.insert(link.target_pc);
                        for (const auto continuation : compiler.Continuations())
                            pending.insert(continuation);
                    }
                    if (backend.trim) backend.trim(backend.context);
                }
            }
            NumericControl() = initial_mxcsr;
            PC() = ISA() == GuestISA::Arm32
                           ? arm32::Location::Make(
                                     image.entry & ~1u, image.entry & 1u, 0, Arm32State().fpscr)
                                     .Key()
                           : image.entry;
            UpdateFPEnvironment();
        }
        if (thread.faults.restore_error)
            swift::ThrowSystemError("restore AOT fault registration", thread.faults.restore_error);
        offline_output = nullptr;
        return output;
    } catch (...) {
        offline_output = nullptr;
        throw;
    }
}

CodeImage CompileGuestImage(std::span<const std::uint8_t> bytes,
                            const std::string& display_path,
                            std::span<const std::uint64_t> entries,
                            bool sweep,
                            RuntimeOptions options,
                            RuntimeStats& stats) {
    options.mode = ExecutionMode::JitOnly;
    options.cache_directory.clear();
    options.artifact_path.clear();
    platform::HostRuntime host{options.decode_timing};
    VM vm{options, stats, host.Bind()};
    auto image = vm.Precompile(bytes, display_path, entries, sweep);
    const auto released = vm.Close();
    if (!released.Complete())
        swift::ThrowSystemError("release AOT compiler mappings",
                                released.unmap_error ? released.unmap_error : released.close_error);
    return image;
}

}  // namespace swift::runtime::execution
