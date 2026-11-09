#include "arm32_user_helpers.h"

#include <cerrno>
#include <span>
#include <sys/mman.h>
#include "base/error.h"
#include "runtime/execution/memory.h"

namespace swift::runtime::platform {
void InstallArm32UserHelpers(execution::GuestMemory& memory) {
    struct Code {
        std::uint32_t offset;
        std::span<const std::uint32_t> words;
    };

#include "arm32_user_helpers.inc"
    // Reserve the complete 64 KiB ABI area, including on 16/64 KiB hosts.
    constexpr std::uint64_t base = 0xffff0000;
    constexpr std::size_t bytes = 0x10000;
    if (memory.Contains(base, bytes, 0))
        swift::ThrowError("guest image overlaps ARM32 kuser area", ErrorKind::InvalidArgument);
    const auto mapped = memory.Map(
            base, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
    if (mapped == UINT64_MAX) swift::ThrowSystemError("map ARM32 kuser page", errno);
    for (const auto& entry : code) {
        if (!memory.CopyOut(base + entry.offset, entry.words.data(), entry.words.size_bytes()))
            swift::ThrowSystemError("populate ARM32 kuser page", errno);
    }
    if (memory.Protect(base, bytes, PROT_READ | PROT_EXEC))
        swift::ThrowSystemError("protect ARM32 kuser page", errno);
}
}  // namespace swift::runtime::platform
