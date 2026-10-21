//
// AOT artifact loader: validate, then install into a live AddressSpace.
//
// Rejection is all-or-nothing, which is where this deliberately differs from
// the JIT disk cache it shares machinery with. The disk cache is an
// optimization: a stale unit is dropped and the JIT recompiles it. An
// artifact *claims to be the program*, so a unit whose guest bytes have moved
// means the artifact belongs to a different binary; installing the rest would
// be running a mixture of two programs. docs/aot-design.md §7.5 asks for
// "reject", not "best effort".
//
#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include "aot/aot_artifact.h"
#include "runtime/common/types.h"

namespace swift::runtime::backend {
class AddressSpace;
}

namespace swift::aot {

class InstalledCodeMap;

struct LoadReport {
    u32 units_installed{};
    u32 dispatch_slots{};
    u64 code_bytes{};
    std::shared_ptr<InstalledCodeMap> mapping;
};

// Install `artifact` into `address_space`. `guest_memory_base`/`mask` describe
// the live guest window and are used to re-hash the guest bytes each unit was
// compiled from.
//
// Returns kAotOk or a SwiftAotStatus. On failure `error` describes why and the
// address space must be considered unusable for this artifact.
int InstallArtifact(swift::runtime::backend::AddressSpace& address_space,
                    const AotImage& artifact,
                    void* guest_memory_base,
                    u64 guest_addr_mask,
                    LoadReport& report,
                    std::string& error);

// Symbol name -> live host entry point, for host->guest calls by name.
// Built from the artifact ELF's own `.symtab`, so it exercises exactly the
// symbol rewrite the writer performed. Symbols that were pointed at the
// failure stub resolve to the stub.
class SymbolIndex {
public:
    bool Build(const std::string& artifact_path,
               const AotImage& artifact,
               std::string& error,
               const std::shared_ptr<InstalledCodeMap>& installed = {});
    // Host address of `name`, or null when it is not a rewritten STT_FUNC.
    // A raw caller must hold an SMC ExecutionScope from before lookup until
    // the generated call returns; a successful lookup alone does not pin code.
    [[nodiscard]] void* Lookup(const std::string& name) const;
    [[nodiscard]] std::size_t Size() const { return by_name.size(); }

private:
    friend int InstallArtifact(swift::runtime::backend::AddressSpace&,
                               const AotImage&,
                               void*,
                               u64,
                               LoadReport&,
                               std::string&);
    std::unordered_map<std::string, u64> by_name;  // name -> artifact code offset
    std::shared_ptr<InstalledCodeMap> mapping;
};

// Runtime mapping from artifact code offset to the host address the unit was
// installed at. Populated by InstallArtifact; consulted by SymbolIndex users.
// Raw execution requires the same SMC execution scope as SymbolIndex::Lookup.
void* ResolveCodeOffset(u64 offset);
void* ResolveCodeOffset(const std::shared_ptr<InstalledCodeMap>& mapping, u64 offset);

}  // namespace swift::aot
