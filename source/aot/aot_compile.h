//
// Offline compiler: guest x86-64 ELF -> AOT artifact.
//
// Function discovery is the whole point of AOT (docs/aot-design.md §3): the
// JIT only ever learns about a function when control reaches it, while the
// `.symtab` hands us every `STT_FUNC` up front. Everything downstream of
// discovery is the *existing* function-mode path -- X86Instance::CompileAt
// runs the same decode/opt/regalloc/emit pipeline the JIT runs, so there is
// no "AOT translation branch" that could drift from it.
//
#pragma once

#include <string>
#include <vector>
#include "aot/aot_artifact.h"

namespace swift::aot {

struct CompileOptions {
    std::string guest_elf;
    std::string out_path;
    bool verbose{};
    // Stop after this many entry addresses (0 = all). Used by the tests to
    // exercise the minimal one-function loop cheaply.
    u32 max_functions{};
};

// Discovered guest function, before compilation.
struct FuncCandidate {
    u64 addr{};
    u64 declared_size{};   // st_size as the guest ELF states it
    u64 inferred_end{};    // boundary used when declared_size == 0
    bool size_inferred{};
    std::string name;      // first name seen at this address
};

// `.symtab` STT_FUNC discovery.
//
// Zero-size policy (docs/aot-design.md §9 asks for an explicit one): a
// zero-size STT_FUNC is *not* dropped and is never treated as a zero-length
// range. Compilation does not need the size at all -- the function-mode
// decoder walks the CFG from the entry address -- so the symbol is compiled
// like any other, and the size is only used for the audit trail. `inferred_end`
// is the next STT_FUNC address above it, clamped to the end of its section;
// `size_inferred` records that this happened. Symbols for which no boundary
// can be inferred keep inferred_end == addr and are still compiled.
bool DiscoverFunctions(const std::string& guest_elf_path,
                       std::vector<FuncCandidate>& out,
                       u32& zero_size_count,
                       std::string& error);

// Full compile. Loads the guest, compiles every discovered function through
// the ordinary function-mode path, collects relocations with
// backend/code_serial.h and writes the artifact ELF.
bool CompileArtifact(const CompileOptions& options, AotStats& stats, std::string& error);

}  // namespace swift::aot
