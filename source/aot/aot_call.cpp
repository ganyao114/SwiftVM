//
// See aot_call.h.
//

#include "aot/aot_call.h"

#include <cstdio>
#include <elfio/elfio.hpp>
#include "fmt/format.h"

#include "guest_call.h"
#include "runtime/backend/address_space.h"
#include "runtime/ir/location.h"
#include "translator/x86/translator.h"

namespace swift::aot {

bool AotGuestEnv::Init(const std::string& guest_elf, const std::string& artifact_path,
                       std::string& error) {
    if (!JitGuestEnv::Init(guest_elf, error)) {
        return false;
    }
    if (!ReadArtifact(artifact_path, artifact_, error)) {
        return false;
    }
    auto* address_space = AddressSpace();
    if (address_space == nullptr) {
        error = "no address space";
        return false;
    }
    // Before any guest code runs: the L2 dispatch replay needs an empty table
    // and the per-unit guest hashes need pristine guest bytes.
    const int status = InstallArtifact(*address_space,
                                       artifact_,
                                       reinterpret_cast<void*>(space().Bias()),
                                       space().Mask(),
                                       report_,
                                       error);
    if (status != kAotOk) {
        error = fmt::format("artifact rejected (status {}): {}", status, error);
        return false;
    }
    if (!index_.Build(artifact_path, artifact_, error)) {
        return false;
    }
    return true;
}

void* AotGuestEnv::EntryOf(const std::string& name, bool& mismatch) const {
    return EntryOfIn(index_, name, mismatch);
}

void* AotGuestEnv::EntryOfIn(const SymbolIndex& index, const std::string& name,
                             bool& mismatch) const {
    mismatch = false;
    void* host = index.Lookup(name);
    if (host == nullptr) {
        return nullptr;
    }
    // The guest address the name stands for. Taken from the guest ELF's own
    // symbol table (JitGuestEnv::RawSymbol), i.e. the input side, so that the
    // comparison below has two independent sources: what the artifact SAYS the
    // symbol compiles to, and what the address space actually publishes for
    // the guest address that symbol had.
    const std::uint64_t guest = RawSymbol(name);
    if (guest == 0) {
        mismatch = true;
        return nullptr;
    }
    auto* space = AddressSpace();
    void* published = space != nullptr ? space->GetCodeCache(runtime::ir::Location{guest})
                                       : nullptr;
    if (published != host) {
        mismatch = true;
        return nullptr;
    }
    return host;
}

std::uint64_t AotGuestEnv::LookupSymbol(const std::string& name) {
    if (auto c = resolved_.find(name); c != resolved_.end()) {
        return c->second;
    }
    bool mismatch = false;
    if (EntryOf(name, mismatch) == nullptr) {
        // Not a compiled STT_FUNC in the artifact (an object, an alias the
        // writer stubbed, or a broken rewrite). Fall back to the guest ELF so
        // objects still resolve, but never paper over a mismatch.
        if (mismatch) {
            resolved_.emplace(name, 0);
            return 0;
        }
        const std::uint64_t addr = JitGuestEnv::LookupSymbol(name);
        resolved_.emplace(name, addr);
        return addr;
    }
    std::uint64_t addr = RawSymbol(name);
    if (addr != 0 && IsIfunc(name)) {
        // The artifact compiled the resolver, because that is what st_value
        // named. Run it through this very call layer, which is also what the
        // guest's own IRELATIVE relocations did during startup.
        guest_call::GuestFn<std::uint64_t()> resolver{this, addr};
        auto r = resolver.Try();
        addr = r.ok() ? r.value() : 0;
    }
    resolved_.emplace(name, addr);
    return addr;
}

std::uint64_t AotGuestEnv::IfuncTrap(const std::string& name) {
    if (!IsIfunc(name)) {
        return 0;
    }
    const std::uint64_t raw = RawSymbol(name);
    if (raw == 0) {
        return 0;
    }
    // Called with a legitimate guest pointer to an empty string would still be
    // wrong-by-plausible-number; the point is that the answer is a code
    // address regardless of the argument, so a zero guest address is enough.
    const guest_call::GuestPtr<const char> nul{std::uint64_t{0}};
    guest_call::GuestFn<std::uint64_t(guest_call::GuestPtr<const char>)> as_if{this, raw};
    auto r = as_if.Try(nul);
    return r.ok() ? r.value() : 0;
}

// --------------------------------------------------------------------------
bool PatchGuestStopByte(const std::string& guest_elf,
                        const std::string& stop_symbol,
                        const std::string& out_path,
                        std::string& error) {
    using namespace ELFIO;
    elfio reader;
    if (!reader.load(guest_elf)) {
        error = "cannot open " + guest_elf;
        return false;
    }
    std::uint64_t vaddr = 0;
    for (Elf_Half i = 0; i < reader.sections.size(); ++i) {
        if (reader.sections[i]->get_type() != SHT_SYMTAB) {
            continue;
        }
        const_symbol_section_accessor syms{reader, reader.sections[i]};
        for (Elf_Xword k = 0; k < syms.get_symbols_num(); ++k) {
            std::string name;
            Elf64_Addr value{};
            Elf_Xword size{};
            unsigned char bind{}, type{}, other{};
            Elf_Half shndx{};
            if (syms.get_symbol(k, name, value, size, bind, type, shndx, other) &&
                name == stop_symbol && type == STT_FUNC) {
                vaddr = value;
                break;
            }
        }
        break;
    }
    if (vaddr == 0) {
        error = "no STT_FUNC named " + stop_symbol + " in " + guest_elf;
        return false;
    }
    // vaddr -> file offset through the PT_LOAD that contains it. Going through
    // the program headers (not the section headers) is what the loader does,
    // so the byte that is patched is the byte that gets mapped.
    std::size_t file_off = 0;
    bool found = false;
    for (Elf_Half i = 0; i < reader.segments.size(); ++i) {
        const auto* seg = reader.segments[i];
        if (seg->get_type() != PT_LOAD) {
            continue;
        }
        const auto va = seg->get_virtual_address();
        if (vaddr >= va && vaddr < va + seg->get_file_size()) {
            file_off = static_cast<std::size_t>(seg->get_offset() + (vaddr - va));
            found = true;
            break;
        }
    }
    if (!found) {
        error = fmt::format("{:#x} is not inside any PT_LOAD file range", vaddr);
        return false;
    }

    std::FILE* in = std::fopen(guest_elf.c_str(), "rb");
    if (in == nullptr) {
        error = "cannot read " + guest_elf;
        return false;
    }
    std::string bytes;
    char buf[65536];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) != 0) {
        bytes.append(buf, n);
    }
    std::fclose(in);
    if (file_off >= bytes.size()) {
        error = "patch offset past end of file";
        return false;
    }
    bytes[file_off] = static_cast<char>(0xF4);  // hlt
    std::FILE* out = std::fopen(out_path.c_str(), "wb");
    if (out == nullptr) {
        error = "cannot write " + out_path;
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    std::fclose(out);
    if (!ok) {
        error = "short write to " + out_path;
        return false;
    }
    return true;
}

}  // namespace swift::aot
