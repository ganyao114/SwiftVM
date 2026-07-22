//
// Guest ARM64 / x86_64 Linux ELF loader (static executables, ET_EXEC + static
// ET_DYN).
//
// Flow: parse the ELF with elfio -> reserve the image span -> copy PT_LOAD
// contents to their *guest* addresses -> locate the in-memory program headers
// for AT_PHDR -> record entry / brk start. SetupInitialStack() then maps the
// guest main stack and builds the classic Linux initial stack frame
// (argc / argv / envp / auxv).
//
// Address modes:
//  - ET_EXEC: the guest runs at its *linked* addresses (position-dependent
//    absolute references work unchanged); the image is reserved at a
//    host-chosen address and the guest->host bias is installed in GuestMemory
//    ("memory_base" mode), on macOS sidestepping the pagezero low-4GB
//    mapping ban.
//  - ET_DYN (static PIE): self-relocating, so it stays on the proven
//    identity path (guest addr == host addr) for now — see loader.cpp.
//

#pragma once

#include <string>
#include <vector>
#include "base/types.h"
#include "guest_memory.h"

namespace swift::linux {

// Guest instruction set, derived from the ELF e_machine field.
enum class GuestISA : u8 { kArm64, kX86_64 };

struct LoadedImage {
    std::string path;
    GuestISA isa{GuestISA::kArm64};
    VAddr entry{};       // guest entry point (linked address + guest load bias)
    VAddr load_bias{};   // GUEST load bias: 0 for ET_EXEC (linked addresses),
                         // host-chosen for identity-mapped static PIE. The
                         // guest->host bias (ET_EXEC) lives in GuestMemory
                         // (memory.GetBias()).
    VAddr phdr{};        // guest address of the in-memory program header table
    u64 phentsize{};
    u64 phnum{};
    VAddr brk_start{};   // initial program break (first guest page past the image)
};

class ElfLoader {
public:
    explicit ElfLoader(GuestMemory* memory) : memory(memory) {}

    // Loads a statically linked ARM64 / x86_64 ELF into guest memory.
    LoadedImage Load(const std::string& path);

private:
    GuestMemory* memory;
};

// Preferred guest main stack placement (guest address; mapped on the host at
// +bias). If the range is not mappable on the host, a host-chosen range is
// used instead (and translated back to a guest address).
inline constexpr u64 kGuestStackTop = 0x7FF00000;
inline constexpr u64 kGuestStackSize = 8_MB;

// Maps the guest stack and writes the Linux ABI initial stack frame
// (argc, argv..., NULL, envp..., NULL, auxv..., AT_NULL) plus the strings /
// AT_RANDOM payload it points to. Returns the initial guest sp.
VAddr SetupInitialStack(GuestMemory& memory,
                        const LoadedImage& image,
                        const std::vector<std::string>& args,
                        const std::vector<std::string>& envs);

}  // namespace swift::linux
