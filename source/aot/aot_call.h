//
// The AOT artifact wired to the host->guest call layer.
//
// This is the shape docs/aot-design.md asks for in §0 ("host programs can call
// guest functions in it by name") and §6, and it is the join between task
// three (the artifact) and task four (GuestFn): a symbol name is resolved
// through the ARTIFACT's own rewritten `.symtab` -- the very table the writer
// produced -- and the call then runs the code the artifact installed.
//
// Two things are deliberately NOT done here.
//
// 1. The call does not branch to the host pointer that SymbolIndex hands back.
//    Entering a compiled unit needs the JIT's entry contract (state register,
//    RSB pointer, dispatcher linkage), and a second implementation of that
//    contract is exactly the kind of divergent path §3 rules out.  Instead the
//    host pointer is used as an ORACLE: EntryOf() requires that the address
//    space's published code for that guest address is the very pointer the
//    artifact's symbol names.  A symbol rewritten to the wrong unit -- the
//    failure that "the symbols were preserved" is supposed to exclude -- fails
//    that check instead of quietly calling the wrong function.
//
// 2. STT_GNU_IFUNC is not resolved by the artifact.  The artifact compiles the
//    RESOLVER, because that is what `st_value` points at, and its rewritten
//    symbol therefore names compiled resolver code.  Calling `strlen` by name
//    without resolving returns a code address: a perfectly plausible number
//    that is not a length.  LookupSymbol() runs the resolver through this same
//    call layer, exactly as JitGuestEnv does, and IfuncTrap() exists so a test
//    can show the unresolved answer really is wrong.
//
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "aot/aot_artifact.h"
#include "aot/aot_load.h"
#include "jit_env.h"

namespace swift::aot {

class AotGuestEnv final : public guest_call::JitGuestEnv {
public:
    // `guest_elf` is loaded into the guest window and `artifact_path` is
    // installed into the address space before any guest code runs.  The
    // artifact must have been compiled, in this process image, from exactly
    // this file: the validity key covers the SwiftVM build id, so an artifact
    // produced by another executable is refused (and rightly so -- every host
    // helper address baked into the code is an offset into that image).
    bool Init(const std::string& guest_elf, const std::string& artifact_path,
              std::string& error);

    [[nodiscard]] const LoadReport& load_report() const { return report_; }
    [[nodiscard]] const AotImage& artifact() const { return artifact_; }
    [[nodiscard]] const SymbolIndex& symbols() const { return index_; }

    // Host entry of `name` per the ARTIFACT's symbol table, cross-checked
    // against the code the address space actually publishes for that guest
    // address.  Returns null when the artifact does not carry the symbol;
    // sets `mismatch` when it carries it but names different code, which is a
    // broken artifact rather than a missing symbol.
    void* EntryOf(const std::string& name, bool& mismatch) const;

    // The same cross-check against an arbitrary index. Exists so a test can
    // build an index from a MUTATED artifact -- one whose `st_value` names a
    // different unit -- and show the check fires. Without it, "the lookup
    // returned a pointer" would be the only evidence, and a wrong pointer
    // looks exactly like a right one.
    void* EntryOfIn(const SymbolIndex& index, const std::string& name, bool& mismatch) const;

    // Guest entry address of `name`, resolved through the artifact.  Overrides
    // JitGuestEnv's guest-ELF lookup so that GuestFn calls really do go
    // through the artifact's symbol table.
    std::uint64_t LookupSymbol(const std::string& name) override;

    // The unresolved answer for an ifunc: what a host would get by calling the
    // artifact's `name` symbol directly.  For `strlen` this is a code address,
    // not a length.  0 when `name` is not an ifunc.
    std::uint64_t IfuncTrap(const std::string& name);

private:
    AotImage artifact_{};
    SymbolIndex index_{};
    LoadReport report_{};
    std::unordered_map<std::string, std::uint64_t> resolved_{};
};

// Writes a copy of `guest_elf` to `out_path` with the first byte of
// `stop_symbol` replaced by `hlt` (0xF4).
//
// The AOT path cannot patch the running image the way JitGuestEnv does: units
// are hashed against live guest bytes when they are installed, so a later
// patch would describe an image that no longer exists (and the install has
// already write-protected the page).  Patching the FILE and compiling the
// artifact from the patched file keeps the artifact a description of the bytes
// that actually run.
bool PatchGuestStopByte(const std::string& guest_elf,
                        const std::string& stop_symbol,
                        const std::string& out_path,
                        std::string& error);

}  // namespace swift::aot
