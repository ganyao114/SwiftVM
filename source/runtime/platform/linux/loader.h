#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "runtime/execution/memory.h"
#include "runtime/arch/guest.h"

namespace swift::runtime::platform {
// Source image identity and initial ABI metadata, not another mapping ledger.
// All segment permissions, replacements and file aliases remain GuestMemory's.
struct LoadedImage {
    std::uint64_t entry{}, phdr{}, load_bias{};
    std::uint16_t phnum{}, phentsize{};
    std::string path;
    std::uint64_t device{}, inode{}, file_size{};
    std::uint16_t elf_type{};
    GuestISA isa{GuestISA::X86_64};
};

LoadedImage LoadELF(execution::GuestMemory&, const std::string&);
LoadedImage LoadELF(execution::GuestMemory&,
                    std::span<const std::uint8_t>,
                    const std::string& display_path);
std::uint64_t InitialStack(execution::GuestMemory&,
                           const LoadedImage&,
                           std::span<const std::string> argv,
                           std::span<const std::string> environment);
}  // namespace swift::runtime::platform
