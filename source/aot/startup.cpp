#include "artifact.h"

#include "base/error.h"
#include "elf.h"
#include "runtime/platform/linux/artifact_file.h"

namespace swift::aot {
bool PatchGuestStopByte(const std::string& guest_path,
                        const std::string& name,
                        const std::string& output,
                        std::string& error) {
    try {
        auto bytes = runtime::platform::ReadArtifactFile(guest_path);
        if (elf::View{bytes}.FileHeader().machine != elf::X86)
            ThrowError("startup byte patching requires an x86-64 guest", ErrorKind::Unsupported);
        const auto symbols = DiscoverSymbols(bytes);
        const Symbol* selected{};
        for (const auto& symbol : symbols) {
            if (symbol.name != name || symbol.type != 2) continue;
            selected = &symbol;
            if (symbol.binding) break;
        }
        if (!selected)
            ThrowError("startup stop symbol is not a defined STT_FUNC", ErrorKind::InvalidArgument);
        const elf::View view{bytes};
        for (const auto& segment : view.Segments()) {
            if (segment.type != elf::Load || !(segment.flags & 1) ||
                selected->guest_value < segment.vaddr ||
                selected->guest_value - segment.vaddr >= segment.filesz)
                continue;
            bytes[segment.offset + selected->guest_value - segment.vaddr] = 0xf4;
            runtime::platform::WriteArtifactFile(output, bytes);
            error.clear();
            return true;
        }
        ThrowError("startup stop symbol has no executable file-backed PT_LOAD byte",
                   ErrorKind::InvalidArgument);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}
}  // namespace swift::aot
