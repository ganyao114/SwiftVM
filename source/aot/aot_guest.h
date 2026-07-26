//
// Guest address space bring-up, shared by `svm_aot compile` and `svm_aot run`.
//
// Two ways in, and they must agree byte for byte -- that agreement is what
// makes the JIT-vs-AOT comparison meaningful:
//
//   Load()          reads the guest ELF and maps its PT_LOADs, exactly the
//                   way source/translator/linux/loader.cpp does.
//   LoadFromImage() maps the PT_LOAD copies the artifact carries.
//
// docs/aot-design.md §1: guest data must land at its *original* guest virtual
// addresses in both cases. Getting that wrong is not a crash, it is "mostly
// works, reads garbage out of globals", which is why the equivalence test
// includes a case that does nothing but read and write guest globals.
//
#pragma once

#include <string>
#include <vector>
#include "aot/aot_artifact.h"
#include "loader.h"

namespace swift::aot {

struct GuestImage {
    swift::linux::GuestMemory memory{};
    swift::linux::LoadedImage image{};
    // Verbatim PT_LOAD descriptors + file bytes, so `compile` can put them in
    // the artifact and `info` can report them.
    std::vector<AotGuestSegment> segments{};

    // Reserve the window (SVM_GUEST_BITS) and install the signal-handler
    // guest-mapping probes. Called by both paths before any mapping.
    bool ReserveWindow(std::string& error);

    bool Load(const std::string& elf_path, std::string& error);
    bool LoadFromImage(const AotImage& artifact, std::string& error);

private:
    bool window_ready{};
};

}  // namespace swift::aot
