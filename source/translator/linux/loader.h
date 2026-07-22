//
// Guest ARM64 Linux ELF loader (static executables, ET_EXEC + static ET_DYN).
//
// Flow: parse the ELF with elfio -> reserve the image span in the guest
// (identity-mapped) address space -> copy PT_LOAD contents -> locate the
// in-memory program headers for AT_PHDR -> record entry / brk start.
// SetupInitialStack() then maps the guest main stack and builds the classic
// Linux initial stack frame (argc / argv / envp / auxv).
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
    VAddr entry{};       // guest entry point (load bias applied)
    VAddr load_bias{};   // 0 for ET_EXEC, reservation base - min_vaddr for ET_DYN
    VAddr phdr{};        // guest address of the in-memory program header table
    u64 phentsize{};
    u64 phnum{};
    VAddr brk_start{};   // initial program break (first host page past the image)
};

class ElfLoader {
public:
    explicit ElfLoader(GuestMemory* memory) : memory(memory) {}

    // Loads a statically linked ARM64 / x86_64 ELF into guest memory.
    LoadedImage Load(const std::string& path);

private:
    GuestMemory* memory;
};

// Preferred guest main stack placement (identity mapped). If the range is
// not mappable on the host (e.g. the low 4GB is off-limits to arm64 macOS
// processes), a host-chosen range is used instead.
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
